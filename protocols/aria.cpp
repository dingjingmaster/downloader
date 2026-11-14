#include "aria.h"

#include <aria2/aria2.h>
#include <clib/cond.h>
#include <clib/directory.h>
#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/path.h>
#include <clib/uri.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

/* One process-wide aria2 engine owns the single libaria2 session on a dedicated
 * thread. Download workers submit tasks and wait for snapshots; every callback
 * therefore runs on the worker thread that owns the task. */
enum DownloadAriaRequest
{
    DOWNLOAD_ARIA_REQUEST_RUN,
    DOWNLOAD_ARIA_REQUEST_PAUSE,
    DOWNLOAD_ARIA_REQUEST_CANCEL
};

enum DownloadEnginePhase
{
    DOWNLOAD_ENGINE_PHASE_RUNNING,
    DOWNLOAD_ENGINE_PHASE_PAUSED,
    DOWNLOAD_ENGINE_PHASE_CANCELLED,
    DOWNLOAD_ENGINE_PHASE_COMPLETED,
    DOWNLOAD_ENGINE_PHASE_FAILED
};

/* Terminal data is cached: libaria2 prunes completed results, so a long-lived
 * session must not depend on getDownloadHandle() for finished GIDs. */
struct DownloadNode
{
    aria2::A2Gid              gid;
    bool                      checked;
    bool                      metadata;
    bool                      complete;
    bool                      hasInfoHash;
    int64_t                   totalLength;
    int64_t                   completedLength;
    std::string               btName;
    std::vector<aria2::FileData> files;
    explicit DownloadNode (aria2::A2Gid value)
        : gid (value), checked (false), metadata (false), complete (false), hasInfoHash (false),
          totalLength (0), completedLength (0) {}
};

struct DownloadEngineTask
{
    CMutex                 mutex;
    CCond                  cond;
    bool                   mutexReady;
    bool                   condReady;
    uint64_t               generation;
    DownloadAriaRequest    request;
    DownloadEnginePhase            phase;
    int                    error;
    uint64_t               total;
    uint64_t               completed;
    uint64_t               speed;
    bool                   hasFileName;
    std::string            fileName;
    std::vector<std::string> payloadFiles;
    std::vector<std::string> metadataFiles;

    /* Set by the worker before submission; owned by the engine afterwards. */
    std::string            url;
    std::string            protocol;
    std::string            stage;
    std::string            privateDirectory;
    std::string            fetched;
    bool                   fetchedDescriptor;
    bool                   metalink;
    std::vector<DownloadNode> nodes;
    aria2::A2Gid           descriptor;
    bool                   fetching;
    bool                   added;
    bool                   stopping;
    DownloadEnginePhase    stopPhase;
    int                    stopError;

    DownloadEngineTask ()
        : mutexReady (false), condReady (false), generation (0), request (DOWNLOAD_ARIA_REQUEST_RUN),
          phase (DOWNLOAD_ENGINE_PHASE_RUNNING), error (0), total (0), completed (0), speed (0), hasFileName (false),
          fetchedDescriptor (false), metalink (false), descriptor (0), fetching (false), added (false),
          stopping (false), stopPhase (DOWNLOAD_ENGINE_PHASE_RUNNING), stopError (0)
    {
        if (!c_mutex_init (&mutex))
            throw std::bad_alloc ();
        mutexReady = true;
        if (!c_cond_init (&cond)) {
            c_mutex_clear (&mutex);
            mutexReady = false;
            throw std::bad_alloc ();
        }
        condReady = true;
    }
    ~DownloadEngineTask ()
    {
        if (condReady)
            c_cond_clear (&cond);
        if (mutexReady)
            c_mutex_clear (&mutex);
    }
};

struct ProtocolTask
{
    DownloadTask           source;
    ProtocolCallbacks      callbacks;
    void*                  userData;
    CMutex                 mutex;
    bool                   mutexReady;
    DownloadAriaRequest    request;
    std::shared_ptr<DownloadEngineTask> engineTask;

    ProtocolTask () : userData (NULL), mutexReady (false), request (DOWNLOAD_ARIA_REQUEST_RUN)
    {
        if (c_mutex_init (&mutex))
            mutexReady = true;
    }
    ~ProtocolTask ()
    {
        if (mutexReady)
            c_mutex_clear (&mutex);
    }
};

namespace {
struct AdapterError
{
    int code;
    explicit AdapterError (int value) : code (value) {}
};

static void require (bool condition, int error = EIO)
{
    if (!condition)
        throw AdapterError (error);
}

static void require_system (int result)
{
    if (result < 0)
        throw AdapterError (errno);
}

/* Reject links and non-regular objects in a resumed stage before aria2 opens
 * anything. The private directory must not be writable by untrusted processes;
 * path-based libaria2 APIs cannot protect against concurrent directory swaps. */
static void check_tree (const std::string& path)
{
    struct stat status;
    require_system (lstat (path.c_str (), &status));
    require (!S_ISLNK (status.st_mode), ELOOP);
    if (S_ISREG (status.st_mode)) {
        require (status.st_nlink == 1, EPERM);
        return;
    }
    require (S_ISDIR (status.st_mode), EINVAL);
    std::unique_ptr<CDirectory, void (*) (CDirectory*)> directory (
        c_directory_open (path.c_str ()), c_directory_free);
    require (directory.get () != NULL, errno);
    for (;;) {
        const char* name = NULL;
        int result = c_directory_next (directory.get (), &name);
        require_system (result);
        if (result == 0)
            break;
        check_tree (path + "/" + name);
    }
}

static bool safe_relative (const std::string& path)
{
    if (path.empty () || path[0] == '/' || path.find ('\\') != std::string::npos ||
        path.find ('\0') != std::string::npos)
        return false;
    size_t start = 0;
    while (start < path.size ()) {
        size_t end = path.find ('/', start);
        std::string part = path.substr (start, end == std::string::npos ? end : end - start);
        if (part.empty () || part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return false;
}

static std::string safe_name (std::string name)
{
    size_t slash = name.find_last_of ("/\\");
    if (slash != std::string::npos)
        name.erase (0, slash + 1);
    for (size_t i = 0; i < name.size (); ++i) {
        if (static_cast<unsigned char> (name[i]) < 32 || name[i] == 127)
            name[i] = '_';
    }
    if (name.empty () || name == "." || name == "..")
        name = "download";
    // Filesystem basename limit (UTF-8 remains untouched unless overlong).
    if (name.size () > 240)
        name = "download";
    return name;
}

static bool descriptor_path (const std::string& path)
{
    const char* suffixes[] = { ".torrent", ".meta4", ".metalink" };
    for (const char* suffix : suffixes) {
        size_t length = strlen (suffix);
        if (path.size () >= length && path.compare (path.size () - length, length, suffix) == 0)
            return true;
    }
    return false;
}

static std::string descriptor_name (std::string source)
{
    source.erase (source.find_first_of ("?#") == std::string::npos ? source.size () : source.find_first_of ("?#"));
    source = safe_name (source);
    const char* suffixes[] = { ".torrent", ".meta4", ".metalink" };
    for (const char* suffix : suffixes) {
        size_t length = strlen (suffix);
        if (source.size () > length && source.compare (source.size () - length, length, suffix) == 0)
            source.resize (source.size () - length);
    }
    return safe_name (source);
}

static std::string local_metadata (const std::string& url)
{
    std::string encoded = url.substr (7);
    if (encoded.compare (0, 10, "localhost/") == 0)
        encoded.erase (0, 9);
    require (!encoded.empty () && encoded[0] == '/', EINVAL);
    char* decoded = NULL;
    size_t length = 0;
    require (c_uri_unescape_string (c_string_view_from_cstr (encoded.c_str ()), &decoded, &length) == 0, EINVAL);
    std::unique_ptr<char, void (*) (void*)> owner (decoded, c_free);
    require (memchr (decoded, '\0', length) == NULL, EINVAL);
    std::string path (decoded, length);
    // Check every ancestor as well as the descriptor itself.
    for (size_t end = 1; end <= path.size (); ++end) {
        if (end != path.size () && path[end] != '/')
            continue;
        struct stat status;
        require_system (lstat (path.substr (0, end).c_str (), &status));
        require (!S_ISLNK (status.st_mode), ELOOP);
        require (end == path.size () ? S_ISREG (status.st_mode) : S_ISDIR (status.st_mode), EINVAL);
    }
    return path;
}

static void append_nodes (std::vector<DownloadNode>& nodes, const std::vector<aria2::A2Gid>& gids)
{
    for (aria2::A2Gid gid : gids) {
        bool found = false;
        for (const DownloadNode& node : nodes)
            found = found || node.gid == gid;
        if (!found)
            nodes.push_back (DownloadNode (gid));
    }
}

static void add_descriptor (aria2::Session* session, const std::string& path, bool metalink,
                            std::vector<DownloadNode>& nodes, const std::string& base,
                            const std::string& directory)
{
    aria2::KeyVals options = { { "pause", "true" }, { "dir", directory }, { "continue", "true" } };
    if (!base.empty ())
        options.push_back ({ "metalink-base-uri", base });
    std::vector<aria2::A2Gid> gids;
    if (metalink) {
        require (aria2::addMetalink (session, &gids, path, options) == 0, EINVAL);
    } else {
        aria2::A2Gid gid = 0;
        require (aria2::addTorrent (session, &gid, path, options) == 0, EINVAL);
        require (!aria2::isNull (gid), EINVAL);
        gids.push_back (gid);
    }
    require (!gids.empty (), EINVAL);
    append_nodes (nodes, gids);
}

static void validate_files (aria2::DownloadHandle* download, const std::string& stage)
{
    for (const aria2::FileData& file : download->getFiles ()) {
        // A magnet waiting for metadata has no actual file path yet.
        if (file.path.empty ())
            continue;
        require (file.path.compare (0, stage.size () + 1, stage + "/") == 0, EPERM);
        std::string relative = file.path.substr (stage.size () + 1);
        require (safe_relative (relative), EPERM);
        require (relative != ".downloader-aria" && relative.compare (0, 17, ".downloader-aria/") != 0, EPERM);
    }
}

static void remove_owned (const std::string& path)
{
    struct stat status;
    if (lstat (path.c_str (), &status) != 0) {
        require (errno == ENOENT, errno);
        return;
    }
    require (S_ISREG (status.st_mode) && status.st_nlink == 1, EPERM);
    require_system (unlink (path.c_str ()));
}

struct DownloadAriaEngine
{
    CMutex                 mutex;
    CCond                  cond;
    bool                   mutexReady;
    bool                   condReady;
    std::thread            thread;
    bool                   started;
    bool                   stopping;
    std::atomic<bool>      failed;
    bool                   initialized;
    aria2::Session*        session;
    std::string            directory;
    int                    lockDescriptor;
    bool                   temporaryDirectory;
    std::vector<std::shared_ptr<DownloadEngineTask>> pending;
    std::vector<std::shared_ptr<DownloadEngineTask>> tasks;

    DownloadAriaEngine ()
        : mutexReady (false), condReady (false), started (false), stopping (false), failed (false),
          initialized (false), session (NULL), lockDescriptor (-1), temporaryDirectory (false)
    {
        if (!c_mutex_init (&mutex))
            throw std::bad_alloc ();
        mutexReady = true;
        if (!c_cond_init (&cond)) {
            c_mutex_clear (&mutex);
            mutexReady = false;
            throw std::bad_alloc ();
        }
        condReady = true;
    }
    ~DownloadAriaEngine ()
    {
        if (condReady)
            c_cond_clear (&cond);
        if (mutexReady)
            c_mutex_clear (&mutex);
    }
};

struct DownloadEngineLock
{
    CMutex* mutex;
    bool    held;
    explicit DownloadEngineLock (CMutex* value) : mutex (value), held (false) { lock (); }
    ~DownloadEngineLock () { unlock (); }
    void lock ()
    {
        if (!held) {
            c_mutex_lock (mutex);
            held = true;
        }
    }
    void unlock ()
    {
        if (held) {
            c_mutex_unlock (mutex);
            held = false;
        }
    }
};

static DownloadAriaEngine& engine_instance ()
{
    static DownloadAriaEngine* instance = new DownloadAriaEngine ();
    return *instance;
}

static void publish_snapshot (DownloadEngineTask& task, DownloadEnginePhase phase, int error, uint64_t total,
                              uint64_t completed, uint64_t speed, bool hasFileName,
                              const std::string& fileName, const std::vector<std::string>* payloadFiles,
                              const std::vector<std::string>* metadataFiles)
{
    DownloadEngineLock guard (&task.mutex);
    task.phase = phase;
    task.error = error;
    task.total = total;
    task.completed = completed;
    task.speed = speed;
    task.hasFileName = hasFileName;
    if (hasFileName)
        task.fileName = fileName;
    if (payloadFiles != NULL)
        task.payloadFiles = *payloadFiles;
    if (metadataFiles != NULL)
        task.metadataFiles = *metadataFiles;
    ++task.generation;
    c_cond_notify_all (&task.cond);
}

static void publish_terminal (DownloadEngineTask& task, DownloadEnginePhase phase, int error)
{
    publish_snapshot (task, phase, error, 0, 0, 0, false, std::string (), NULL, NULL);
}

static std::vector<aria2::A2Gid> owned_gids (const DownloadEngineTask& task)
{
    std::vector<aria2::A2Gid> values;
    if (task.fetching && !aria2::isNull (task.descriptor))
        values.push_back (task.descriptor);
    for (const DownloadNode& node : task.nodes)
        values.push_back (node.gid);
    std::vector<aria2::A2Gid> result;
    for (aria2::A2Gid gid : values) {
        if (std::find (result.begin (), result.end (), gid) == result.end ())
            result.push_back (gid);
    }
    return result;
}

static std::string engine_directory (int* lockDescriptor, bool* temporary)
{
    *lockDescriptor = -1;
    *temporary = false;
    std::string base;
    const char* cache = getenv ("XDG_CACHE_HOME");
    if (cache != NULL && cache[0] == '/')
        base = cache;
    else {
        const char* home = getenv ("HOME");
        if (home != NULL && home[0] == '/')
            base = std::string (home) + "/.cache";
    }
    if (!base.empty ()) {
        std::string path = base + "/downloader/aria2";
        struct stat status;
        if (c_directory_create_parents (path.c_str ()) == 0 && lstat (path.c_str (), &status) == 0 &&
            S_ISDIR (status.st_mode) && !S_ISLNK (status.st_mode) && chmod (path.c_str (), 0700) == 0) {
            int descriptor = open ((path + "/engine.lock").c_str (), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (descriptor >= 0 && flock (descriptor, LOCK_EX | LOCK_NB) == 0) {
                *lockDescriptor = descriptor;
                return path;
            }
            if (descriptor >= 0)
                close (descriptor);
        }
    }
    /* Another daemon owns the shared DHT state; use a private engine directory. */
    char templatePath[] = "/tmp/downloader-aria-XXXXXX";
    char* created = mkdtemp (templatePath);
    if (created == NULL)
        return std::string ();
    *temporary = true;
    return std::string (created);
}

static void engine_cleanup_directory (DownloadAriaEngine& engine)
{
    if (engine.temporaryDirectory && !engine.directory.empty ())
        (void)c_directory_remove_recursive (engine.directory.c_str ());
    if (engine.lockDescriptor >= 0)
        (void)close (engine.lockDescriptor);
    engine.lockDescriptor = -1;
    engine.directory.clear ();
    engine.temporaryDirectory = false;
}

static void engine_loop (DownloadAriaEngine* engine);

static int engine_start_locked (DownloadAriaEngine& engine)
{
    if (engine.started)
        return 0;
    if (engine.failed.load ()) {
        errno = EIO;
        return -1;
    }
    engine.directory = engine_directory (&engine.lockDescriptor, &engine.temporaryDirectory);
    if (engine.directory.empty ()) {
        errno = EIO;
        return -1;
    }
    if (!engine.initialized) {
        if (aria2::libraryInit () != 0) {
            engine_cleanup_directory (engine);
            errno = EIO;
            return -1;
        }
        engine.initialized = true;
    }
    aria2::KeyVals options = {
        { "no-conf", "true" }, { "no-netrc", "true" }, { "enable-rpc", "false" },
        { "check-certificate", "true" }, { "continue", "true" },
        { "auto-file-renaming", "false" }, { "allow-overwrite", "false" },
        { "check-integrity", "true" }, { "seed-time", "0" }, { "seed-ratio", "0.0" },
        { "follow-torrent", "mem" }, { "follow-metalink", "mem" }, { "pause-metadata", "true" },
        { "bt-load-saved-metadata", "false" },
        { "bt-save-metadata", "false" }, { "bt-enable-lpd", "false" },
        { "enable-dht", "true" }, { "enable-dht6", "false" },
        { "dht-file-path", engine.directory + "/dht" }, { "dht-file-path6", engine.directory + "/dht6" },
        { "max-download-result", "100" }, { "auto-save-interval", "1" },
        { "max-concurrent-downloads", "64" },
        { "file-allocation", "none" }, { "console-log-level", "error" },
        { "summary-interval", "0" }, { "download-result", "hide" }
    };
    aria2::SessionConfig config;
    config.useSignalHandler = false;
    config.keepRunning = true;
    engine.session = aria2::sessionNew (options, config);
    if (engine.session == NULL) {
        (void)aria2::libraryDeinit ();
        engine.initialized = false;
        engine_cleanup_directory (engine);
        errno = EIO;
        return -1;
    }
    engine.stopping = false;
    try {
        engine.thread = std::thread (engine_loop, &engine);
    } catch (...) {
        (void)aria2::sessionFinal (engine.session);
        engine.session = NULL;
        (void)aria2::libraryDeinit ();
        engine.initialized = false;
        engine_cleanup_directory (engine);
        errno = EAGAIN;
        return -1;
    }
    engine.started = true;
    return 0;
}

static int engine_enqueue (DownloadAriaEngine& engine, const std::shared_ptr<DownloadEngineTask>& task)
{
    int result;

    DownloadEngineLock guard (&engine.mutex);
    result = engine_start_locked (engine);
    if (result == 0) {
        engine.pending.push_back (task);
        c_cond_notify_all (&engine.cond);
    }
    return result;
}

static int engine_task_add (DownloadAriaEngine& engine, DownloadEngineTask& task)
{
    aria2::Session* session = engine.session;
    if (task.url.compare (0, 7, "magnet:") == 0) {
        aria2::A2Gid gid = 0;
        aria2::KeyVals options = { { "dir", task.stage }, { "continue", "true" }, { "pause", "true" } };
        if (aria2::addUri (session, &gid, { task.url }, options) != 0 || aria2::isNull (gid))
            return EINVAL;
        task.nodes.push_back (DownloadNode (gid));
        task.nodes.back ().metadata = true;
    } else if (task.url.compare (0, 7, "file://") == 0) {
        add_descriptor (session, local_metadata (task.url), task.metalink, task.nodes, "", task.stage);
    } else {
        aria2::KeyVals options = {
            { "dir", task.privateDirectory }, { "out", "descriptor" },
            { "follow-torrent", "false" }, { "follow-metalink", "false" },
            { "enable-http-pipelining", "false" }
        };
        if (aria2::addUri (session, &task.descriptor, { task.url }, options) != 0 ||
            aria2::isNull (task.descriptor))
            return EINVAL;
        task.fetching = true;
    }
    return 0;
}

/* Returns false once the task reached a terminal phase and was signalled. */
static bool engine_task_update (DownloadAriaEngine& engine, const std::shared_ptr<DownloadEngineTask>& task)
{
    aria2::Session* session = engine.session;
    DownloadAriaRequest request;

    c_mutex_lock (&task->mutex);
    request = task->request;
    c_mutex_unlock (&task->mutex);

    if (request != DOWNLOAD_ARIA_REQUEST_RUN && !task->stopping) {
        task->stopping = true;
        task->stopPhase =
            request == DOWNLOAD_ARIA_REQUEST_PAUSE ? DOWNLOAD_ENGINE_PHASE_PAUSED : DOWNLOAD_ENGINE_PHASE_CANCELLED;
        for (aria2::A2Gid gid : owned_gids (*task))
            (void)aria2::removeDownload (session, gid, true);
    }
    if (task->stopping) {
        bool removed = true;
        for (size_t index = 0; index < task->nodes.size (); ++index) {
            aria2::A2Gid gid = task->nodes[index].gid;
            std::unique_ptr<aria2::DownloadHandle, void (*) (aria2::DownloadHandle*)> current (
                aria2::getDownloadHandle (session, gid), aria2::deleteDownloadHandle);
            if (!current)
                continue;
            /* A descriptor can complete in the same iteration in which pause is
             * observed and spawn paused followedBy/belongsTo children. Those
             * children are not removed by removing the parent, so discover and
             * remove them transitively before declaring the task stopped. */
            std::vector<aria2::A2Gid> children = current->getFollowedBy ();
            aria2::A2Gid parent = current->getBelongsTo ();
            if (!aria2::isNull (parent))
                append_nodes (task->nodes, { parent });
            if (!children.empty ())
                append_nodes (task->nodes, children);
            (void)aria2::removeDownload (session, gid, true);
            aria2::DownloadStatus status = current->getStatus ();
            if (status != aria2::DOWNLOAD_REMOVED && status != aria2::DOWNLOAD_ERROR &&
                status != aria2::DOWNLOAD_COMPLETE)
                removed = false;
        }
        if (!removed)
            return true;
        publish_terminal (*task, task->stopPhase,
                          task->stopPhase == DOWNLOAD_ENGINE_PHASE_FAILED ? task->stopError : 0);
        return false;
    }

    if (task->fetching) {
        std::unique_ptr<aria2::DownloadHandle, void (*) (aria2::DownloadHandle*)> download (
            aria2::getDownloadHandle (session, task->descriptor), aria2::deleteDownloadHandle);
        require (download.get () != NULL, EIO);
        aria2::DownloadStatus status = download->getStatus ();
        require (status != aria2::DOWNLOAD_ERROR && status != aria2::DOWNLOAD_REMOVED, EIO);
        if (status == aria2::DOWNLOAD_COMPLETE) {
            download.reset ();
            add_descriptor (session, task->fetched, task->metalink, task->nodes, task->url, task->stage);
            task->fetching = false;
        }
    }

    uint64_t total = 0, completed = 0, speed = 0;
    bool done = !task->fetching && !task->nodes.empty ();
    bool hasName = false;
    std::string name;
    size_t payloadCount = 0;
    std::set<std::string> payloadFiles;
    std::set<std::string> metadataFiles;
    std::set<std::string> controlFiles;
    for (size_t index = 0; index < task->nodes.size (); ++index) {
        if (!task->nodes[index].complete) {
            /* append_nodes() may reallocate task->nodes; re-index after it. */
            DownloadNode& node = task->nodes[index];
            std::unique_ptr<aria2::DownloadHandle, void (*) (aria2::DownloadHandle*)> download (
                aria2::getDownloadHandle (session, node.gid), aria2::deleteDownloadHandle);
            require (download.get () != NULL, EIO);
            std::vector<aria2::A2Gid> children = download->getFollowedBy ();
            // Metalink metaurl torrents are auxiliary metadata with belongsTo,
            // rather than followedBy. Their parent is the actual payload task.
            aria2::A2Gid parent = download->getBelongsTo ();
            bool auxiliary = !aria2::isNull (parent);
            if (auxiliary) {
                node.metadata = true;
                append_nodes (task->nodes, { parent });
            }
            if (!children.empty ()) {
                /* append_nodes() may reallocate; re-index instead of reusing
                 * the reference obtained before the first append. */
                task->nodes[index].metadata = true;
                append_nodes (task->nodes, children);
            }
            DownloadNode& current = task->nodes[index];
            // Post-download parse errors can be swallowed by libaria2 while
            // leaving the descriptor status COMPLETE. A recognized descriptor
            // is never a terminal payload, even if no children were generated.
            current.hasInfoHash = !download->getInfoHash ().empty ();
            if (!current.hasInfoHash) {
                for (const aria2::FileData& file : download->getFiles ()) {
                    if (descriptor_path (file.path))
                        current.metadata = true;
                }
            }
            aria2::DownloadStatus status = download->getStatus ();
            require (status != aria2::DOWNLOAD_ERROR && status != aria2::DOWNLOAD_REMOVED, EIO);
            if (!current.checked) {
                if (!current.metadata)
                    validate_files (download.get (), task->stage);
                if (status == aria2::DOWNLOAD_PAUSED)
                    require (aria2::unpauseDownload (session, current.gid) == 0);
                current.checked = true;
            }
            if (status != aria2::DOWNLOAD_COMPLETE) {
                done = false;
                if (status == aria2::DOWNLOAD_ACTIVE || status == aria2::DOWNLOAD_WAITING ||
                    status == aria2::DOWNLOAD_PAUSED) {
                    std::string btName = download->getBtMetaInfo ().name;
                    if (!btName.empty ())
                        current.btName = btName;
                }
                speed += static_cast<uint64_t> (std::max (0, download->getDownloadSpeed ()));
            } else {
                current.complete = true;
                if (current.metadata)
                    require (auxiliary || !children.empty (), EINVAL);
            }
            current.files = download->getFiles ();
            current.totalLength = std::max<int64_t> (0, download->getTotalLength ());
            current.completedLength = std::max<int64_t> (0, download->getCompletedLength ());
        }
        DownloadNode& node = task->nodes[index];
        if (node.metadata) {
            // Prefer memory, but aria2 may keep known-length followed
            // descriptors on disk. Track only exact paths reported by a
            // metadata node, never infer ownership from a file suffix.
            for (const aria2::FileData& file : node.files) {
                if (file.path.compare (0, task->stage.size () + 1, task->stage + "/") == 0) {
                    require (safe_relative (file.path.substr (task->stage.size () + 1)), EPERM);
                    metadataFiles.insert (file.path);
                }
            }
            continue;
        }
        ++payloadCount;
        for (const aria2::FileData& file : node.files) {
            if (!file.path.empty ()) {
                payloadFiles.insert (file.path);
                // Multi-file torrents use <torrent-root>.aria2, outside
                // their nested payload directory but still inside stage.
                size_t slash = file.path.find ('/', task->stage.size () + 1);
                if (slash != std::string::npos && node.hasInfoHash)
                    controlFiles.insert (file.path.substr (0, slash) + ".aria2");
                else
                    controlFiles.insert (file.path + ".aria2");
            }
        }
        total += static_cast<uint64_t> (node.totalLength);
        completed += static_cast<uint64_t> (node.completedLength);
        name = safe_name (node.btName.empty () ? descriptor_name (task->url) : node.btName);
        hasName = true;
    }
    for (const std::string& control : controlFiles)
        require (payloadFiles.count (control) == 0, EINVAL);

    std::vector<std::string> payloadList (payloadFiles.begin (), payloadFiles.end ());
    std::vector<std::string> metadataList (metadataFiles.begin (), metadataFiles.end ());
    publish_snapshot (*task, DOWNLOAD_ENGINE_PHASE_RUNNING, 0, total, completed, speed, hasName, name, NULL, NULL);
    if (done) {
        /* A pause that arrived while the last piece finished must not be
         * reported as completion; the core would keep the task paused anyway. */
        DownloadAriaRequest finalRequest;
        c_mutex_lock (&task->mutex);
        finalRequest = task->request;
        c_mutex_unlock (&task->mutex);
        if (finalRequest != DOWNLOAD_ARIA_REQUEST_RUN) {
            for (aria2::A2Gid gid : owned_gids (*task))
                (void)aria2::removeDownload (session, gid, true);
            publish_terminal (*task,
                              finalRequest == DOWNLOAD_ARIA_REQUEST_PAUSE ? DOWNLOAD_ENGINE_PHASE_PAUSED
                                                                          : DOWNLOAD_ENGINE_PHASE_CANCELLED,
                              0);
            return false;
        }
        require (payloadCount > 0, EINVAL);
        for (aria2::A2Gid gid : owned_gids (*task))
            (void)aria2::removeDownload (session, gid, true);
        publish_snapshot (*task, DOWNLOAD_ENGINE_PHASE_COMPLETED, 0, total, completed, speed, hasName, name,
                          &payloadList, &metadataList);
        return false;
    }
    return true;
}

static void engine_fail_all (DownloadAriaEngine& engine, int error)
{
    for (const std::shared_ptr<DownloadEngineTask>& task : engine.tasks) {
        try {
            publish_terminal (*task, DOWNLOAD_ENGINE_PHASE_FAILED, error);
        } catch (...) {
        }
    }
    for (const std::shared_ptr<DownloadEngineTask>& task : engine.pending) {
        try {
            publish_terminal (*task, DOWNLOAD_ENGINE_PHASE_FAILED, error);
        } catch (...) {
        }
    }
    engine.tasks.clear ();
    engine.pending.clear ();
}

static void engine_loop_body (DownloadAriaEngine* engine)
{
    DownloadEngineLock guard (&engine->mutex);
    while (!engine->stopping) {
        if (engine->pending.empty () && engine->tasks.empty ()) {
            c_cond_wait (&engine->cond, &engine->mutex);
            continue;
        }
        for (const std::shared_ptr<DownloadEngineTask>& task : engine->pending)
            engine->tasks.push_back (task);
        engine->pending.clear ();

        bool any = false;
        for (size_t index = 0; index < engine->tasks.size ();) {
            std::shared_ptr<DownloadEngineTask> task = engine->tasks[index];
            if (!task->added) {
                int addResult;
                try {
                    addResult = engine_task_add (*engine, *task);
                } catch (const AdapterError& failure) {
                    addResult = failure.code;
                } catch (...) {
                    addResult = EIO;
                }
                if (addResult != 0) {
                    for (aria2::A2Gid gid : owned_gids (*task))
                        (void)aria2::removeDownload (engine->session, gid, true);
                    publish_terminal (*task, DOWNLOAD_ENGINE_PHASE_FAILED, addResult);
                    engine->tasks.erase (engine->tasks.begin () + static_cast<std::ptrdiff_t> (index));
                    continue;
                }
                task->added = true;
            }
            any = true;
            ++index;
        }
        guard.unlock ();
        int runResult = any ? aria2::run (engine->session, aria2::RUN_ONCE) : 0;
        guard.lock ();
        if (runResult < 0) {
            engine->failed.store (true);
            engine_fail_all (*engine, EIO);
            engine->stopping = true;
            break;
        }
        for (size_t index = 0; index < engine->tasks.size ();) {
            std::shared_ptr<DownloadEngineTask> task = engine->tasks[index];
            bool keep;
            try {
                keep = engine_task_update (*engine, task);
            } catch (const AdapterError& failure) {
                /* Reuse the stop state machine: it removes owned GIDs and their
                 * transitive children before reporting the failure, so a failed
                 * task cannot keep downloading into a stage the core may delete. */
                task->stopping = true;
                task->stopPhase = DOWNLOAD_ENGINE_PHASE_FAILED;
                task->stopError = failure.code;
                for (aria2::A2Gid gid : owned_gids (*task))
                    (void)aria2::removeDownload (engine->session, gid, true);
                keep = true;
            } catch (const std::bad_alloc&) {
                task->stopping = true;
                task->stopPhase = DOWNLOAD_ENGINE_PHASE_FAILED;
                task->stopError = ENOMEM;
                for (aria2::A2Gid gid : owned_gids (*task))
                    (void)aria2::removeDownload (engine->session, gid, true);
                keep = true;
            } catch (...) {
                task->stopping = true;
                task->stopPhase = DOWNLOAD_ENGINE_PHASE_FAILED;
                task->stopError = EIO;
                for (aria2::A2Gid gid : owned_gids (*task))
                    (void)aria2::removeDownload (engine->session, gid, true);
                keep = true;
            }
            if (keep)
                ++index;
            else
                engine->tasks.erase (engine->tasks.begin () + static_cast<std::ptrdiff_t> (index));
        }
    }
}

static void engine_loop (DownloadAriaEngine* engine)
{
    try {
        engine_loop_body (engine);
    } catch (...) {
        engine->failed.store (true);
    }
}

static void engine_stop ()
{
    DownloadAriaEngine& engine = engine_instance ();
    {
        DownloadEngineLock guard (&engine.mutex);
        if (!engine.started) {
            engine_cleanup_directory (engine);
            if (engine.initialized) {
                (void)aria2::libraryDeinit ();
                engine.initialized = false;
            }
            return;
        }
        engine.stopping = true;
        c_cond_notify_all (&engine.cond);
    }
    if (engine.thread.joinable ())
        engine.thread.join ();
    {
        DownloadEngineLock guard (&engine.mutex);
        engine.tasks.clear ();
        engine.pending.clear ();
        if (engine.session != NULL) {
            aria2::Session* session = engine.session;
            (void)aria2::shutdown (session, true);
            for (int iteration = 0; iteration < 10; ++iteration) {
                int result = aria2::run (session, aria2::RUN_ONCE);
                if (result <= 0)
                    break;
            }
            (void)aria2::sessionFinal (session);
            engine.session = NULL;
        }
        if (engine.initialized) {
            (void)aria2::libraryDeinit ();
            engine.initialized = false;
        }
        engine_cleanup_directory (engine);
        engine.started = false;
        engine.stopping = false;
    }
}

static void progress (ProtocolTask* task, uint64_t total, uint64_t completed, uint64_t speed)
{
    if (task->callbacks.onProgress)
        task->callbacks.onProgress (task->userData, total, completed, speed);
}

static bool prepare_stage (ProtocolTask* task, std::string& stage, std::string& privateDirectory,
                           std::string& fetched)
{
    std::unique_ptr<char, void (*) (void*)> canonical (
        c_path_canonicalize (task->source.tempPath, NULL), c_free);
    require (canonical.get () != NULL, errno);
    stage = canonical.get ();
    require (!stage.empty () && stage[0] == '/' && stage != "/", EINVAL);
    require (safe_relative (stage.substr (1)), EINVAL);
    if (c_directory_create_with_mode (stage.c_str (), 0700) != 0)
        require (errno == EEXIST, errno);
    struct stat stageStatus;
    require_system (lstat (stage.c_str (), &stageStatus));
    require (S_ISDIR (stageStatus.st_mode), ELOOP);
    // A user-selected parent may legitimately be a symlink. Resolve it once;
    // the stage itself and everything inside it must remain link-free.
    char resolved[PATH_MAX];
    char* resolvedResult = realpath (stage.c_str (), resolved);
    require (resolvedResult != NULL, errno);
    stage = resolved;
    check_tree (stage);
    privateDirectory = stage + "/.downloader-aria";
    if (c_directory_create_with_mode (privateDirectory.c_str (), 0700) != 0)
        require (errno == EEXIST, errno);
    fetched = privateDirectory + "/descriptor";
    return true;
}

static void finalize_stage (const std::string& stage, const std::string& privateDirectory,
                            const std::string& fetched, bool fetchedDescriptor,
                            const std::vector<std::string>& payloadFiles,
                            const std::vector<std::string>& metadataFiles)
{
    check_tree (stage);
    require (!payloadFiles.empty (), EINVAL);
    for (const std::string& path : payloadFiles) {
        struct stat status;
        require_system (lstat (path.c_str (), &status));
        require (S_ISREG (status.st_mode) && status.st_nlink == 1, EINVAL);
    }
    std::set<std::string> payloadSet (payloadFiles.begin (), payloadFiles.end ());
    for (const std::string& path : metadataFiles) {
        require (payloadSet.count (path) == 0, EINVAL);
        remove_owned (path);
        // A later child can legitimately be named descriptor.aria2.
        if (payloadSet.count (path + ".aria2") == 0)
            remove_owned (path + ".aria2");
    }
    if (fetchedDescriptor) {
        remove_owned (fetched);
        remove_owned (fetched + ".aria2");
    }
    /* Pre-upgrade adapters kept per-task DHT state inside this directory.
     * Remove it so a resumed legacy stage can still commit. */
    remove_owned (privateDirectory + "/dht");
    remove_owned (privateDirectory + "/dht6");
    remove_owned (privateDirectory + "/dht__temp");
    remove_owned (privateDirectory + "/dht6__temp");
    require_system (c_directory_remove (privateDirectory.c_str ()));
}

static int execute (ProtocolTask* task)
{
    DownloadAriaEngine& engine = engine_instance ();
    std::string stage;
    std::string privateDirectory;
    std::string fetched;
    prepare_stage (task, stage, privateDirectory, fetched);

    std::shared_ptr<DownloadEngineTask> engineTask;
    try {
        engineTask.reset (new DownloadEngineTask ());
    } catch (...) {
        throw AdapterError (ENOMEM);
    }
    engineTask->url = task->source.resourceUrl;
    engineTask->protocol = task->source.protocol;
    engineTask->metalink = strcmp (task->source.protocol, "metalink") == 0;
    engineTask->stage = stage;
    engineTask->privateDirectory = privateDirectory;
    engineTask->fetched = fetched;
    bool magnet = std::string (task->source.resourceUrl).compare (0, 7, "magnet:") == 0;
    engineTask->fetchedDescriptor =
        !magnet && std::string (task->source.resourceUrl).compare (0, 7, "file://") != 0;
    {
        c_mutex_lock (&task->mutex);
        engineTask->request = task->request;
        task->engineTask = engineTask;
        c_mutex_unlock (&task->mutex);
    }
    progress (task, task->source.totalBytes, task->source.downloadedBytes, 0);
    if (engine_enqueue (engine, engineTask) != 0) {
        c_mutex_lock (&task->mutex);
        task->engineTask.reset ();
        c_mutex_unlock (&task->mutex);
        throw AdapterError (errno ? errno : EIO);
    }
    if (task->callbacks.onStarted)
        task->callbacks.onStarted (task->userData);

    uint64_t seen = 0;
    std::string announced;
    for (;;) {
        DownloadEnginePhase phase;
        int error;
        uint64_t total, completed, speed;
        bool hasFileName;
        std::string fileName;
        std::vector<std::string> payloadFiles;
        std::vector<std::string> metadataFiles;
        c_mutex_lock (&engineTask->mutex);
        while (engineTask->generation == seen && !engine.failed.load ()) {
            if (c_cond_wait_timeout (&engineTask->cond, &engineTask->mutex, 500) < 0)
                break;
        }
        if (engineTask->generation == seen) {
            c_mutex_unlock (&engineTask->mutex);
            throw AdapterError (EIO);
        }
        seen = engineTask->generation;
        phase = engineTask->phase;
        error = engineTask->error;
        total = engineTask->total;
        completed = engineTask->completed;
        speed = engineTask->speed;
        hasFileName = engineTask->hasFileName;
        fileName = engineTask->fileName;
        payloadFiles = engineTask->payloadFiles;
        metadataFiles = engineTask->metadataFiles;
        c_mutex_unlock (&engineTask->mutex);

        if (phase == DOWNLOAD_ENGINE_PHASE_RUNNING) {
            progress (task, total, completed, speed);
            if (hasFileName && fileName != announced) {
                announced = fileName;
                if (task->callbacks.onFileName)
                    task->callbacks.onFileName (task->userData, fileName.c_str ());
            }
            continue;
        }
        if (phase == DOWNLOAD_ENGINE_PHASE_PAUSED)
            return 1;
        if (phase == DOWNLOAD_ENGINE_PHASE_CANCELLED)
            throw AdapterError (ECANCELED);
        if (phase == DOWNLOAD_ENGINE_PHASE_FAILED)
            throw AdapterError (error > 0 ? error : EIO);
        finalize_stage (stage, privateDirectory, fetched, engineTask->fetchedDescriptor, payloadFiles,
                        metadataFiles);
        return 0;
    }
}

static bool aria_can_handle (const char* url, const char* protocol)
{
    if (!url || !protocol)
        return false;
    bool supported = !strcmp (protocol, "bittorrent") || !strcmp (protocol, "magnet") || !strcmp (protocol, "metalink");
    return supported && (!strncmp (url, "magnet:", 7) || !strncmp (url, "http://", 7) ||
                         !strncmp (url, "https://", 8) || !strncmp (url, "file://", 7));
}

static int aria_open (ProtocolTask** result, const DownloadTask* source, const ProtocolCallbacks* callbacks, void* userData)
{
    if (result)
        *result = NULL;
    if (!result || !source || !memchr (source->resourceUrl, '\0', sizeof (source->resourceUrl)) ||
        !memchr (source->protocol, '\0', sizeof (source->protocol)) ||
        !memchr (source->tempPath, '\0', sizeof (source->tempPath)) || source->tempPath[0] == '\0' ||
        !aria_can_handle (source->resourceUrl, source->protocol)) {
        errno = EINVAL;
        return -1;
    }
    ProtocolTask* task = new (std::nothrow) ProtocolTask ();
    if (!task) {
        errno = ENOMEM;
        return -1;
    }
    if (!task->mutexReady) {
        delete task;
        errno = ENOMEM;
        return -1;
    }
    task->source = *source;
    if (callbacks)
        task->callbacks = *callbacks;
    task->userData = userData;
    *result = task;
    return 0;
}

static int aria_run (ProtocolTask* task)
{
    if (!task) {
        errno = EINVAL;
        return -1;
    }
    int error = EIO;
    try {
        int result = execute (task);
        if (result == 0) {
            if (task->callbacks.onCompleted)
                task->callbacks.onCompleted (task->userData);
            return 0;
        }
        if (result == 1) {
            if (task->callbacks.onPaused)
                task->callbacks.onPaused (task->userData);
            return 1;
        }
        error = ECANCELED;
    } catch (const AdapterError& failure) {
        error = failure.code;
    } catch (const std::bad_alloc&) {
        error = ENOMEM;
    } catch (...) {
        error = EIO;
    }
    if (task->callbacks.onFailed) {
        try { task->callbacks.onFailed (task->userData, error, strerror (error)); } catch (...) {}
    }
    errno = error;
    return -1;
}

static int set_request (ProtocolTask* task, DownloadAriaRequest value)
{
    std::shared_ptr<DownloadEngineTask> engineTask;

    if (!task) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&task->mutex);
    if (task->request != DOWNLOAD_ARIA_REQUEST_CANCEL)
        task->request = value;
    engineTask = task->engineTask;
    c_mutex_unlock (&task->mutex);
    if (engineTask) {
        c_mutex_lock (&engineTask->mutex);
        if (engineTask->request != DOWNLOAD_ARIA_REQUEST_CANCEL)
            engineTask->request = value;
        c_mutex_unlock (&engineTask->mutex);
    }
    return 0;
}

static int aria_pause (ProtocolTask* task) { return set_request (task, DOWNLOAD_ARIA_REQUEST_PAUSE); }
static int aria_resume (ProtocolTask* task) { return set_request (task, DOWNLOAD_ARIA_REQUEST_RUN); }
static int aria_cancel (ProtocolTask* task) { return set_request (task, DOWNLOAD_ARIA_REQUEST_CANCEL); }

static void aria_close (ProtocolTask* task)
{
    delete task;
}

static bool aria_retryable (ProtocolTask* task, int errorCode, const char* message)
{
    (void)task;
    (void)message;
    if (errorCode == EIO || errorCode == EAGAIN || errorCode == ETIMEDOUT || errorCode == ECONNRESET ||
        errorCode == ECONNREFUSED || errorCode == EHOSTUNREACH || errorCode == ENETUNREACH ||
        errorCode == ENETDOWN || errorCode == EPIPE || errorCode == ECONNABORTED)
        return true;
    return false;
}

static void aria_finish (void)
{
    try {
        engine_stop ();
    } catch (...) {
    }
}
} // namespace

extern "C" const ProtocolOps* aria_protocol_ops (void)
{
    static const ProtocolOps operations = {
        "aria2", aria_can_handle, aria_open, aria_run, aria_pause, aria_resume, aria_cancel, NULL, aria_close,
        aria_finish, aria_retryable, NULL
    };
    return &operations;
}
