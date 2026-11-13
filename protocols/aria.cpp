#include "aria.h"

#include <aria2/aria2.h>
#include <clib/directory.h>
#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/path.h>
#include <clib/uri.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <limits.h>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

/* C++ containers are used only at the libaria2 boundary. No aria2 object escapes
 * run, including when a callback requests pause/cancel on this worker thread. */
enum DownloadAriaRequest
{
    DOWNLOAD_ARIA_REQUEST_RUN,
    DOWNLOAD_ARIA_REQUEST_PAUSE,
    DOWNLOAD_ARIA_REQUEST_CANCEL
};

struct ProtocolTask
{
    DownloadTask           source;
    ProtocolCallbacks      callbacks;
    void*                  userData;
    CMutex                 mutex;
    DownloadAriaRequest    request;
    uint64_t               lastTotal;
    uint64_t               lastCompleted;
    uint64_t               lastSpeed;
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

struct Runtime
{
    CMutex mutex;
    bool initialized;
    Runtime () : mutex (), initialized (false)
    {
        require (c_mutex_init (&mutex), ENOMEM);
    }
    ~Runtime ()
    {
        // Application must join workers before process teardown.
        if (initialized) {
            try { aria2::libraryDeinit (); } catch (...) {}
        }
        c_mutex_clear (&mutex);
    }
};

static Runtime& runtime ()
{
    static Runtime instance;
    return instance;
}

static DownloadAriaRequest request (ProtocolTask* task)
{
    c_mutex_lock (&task->mutex);
    DownloadAriaRequest value = task->request;
    c_mutex_unlock (&task->mutex);
    return value;
}

static void progress (ProtocolTask* task, uint64_t total = 0, uint64_t completed = 0, uint64_t speed = 0)
{
    task->lastTotal = total;
    task->lastCompleted = completed;
    task->lastSpeed = speed;
    if (task->callbacks.onProgress)
        task->callbacks.onProgress (task->userData, total, completed, speed);
}

struct SessionOwner
{
    Runtime& owner;
    bool locked;
    ProtocolTask* task;
    aria2::Session* session;
    SessionOwner (Runtime& value, ProtocolTask* context)
        : owner (value), locked (false), task (context), session (NULL) {}
    ~SessionOwner ()
    {
        // Normal exits use finish(), which reports shutdown/finalization errors.
        // Exception cleanup must not throw through the C ABI.
        if (session) {
            try { finish (); } catch (...) {}
        }
        if (locked)
            c_mutex_unlock (&owner.mutex);
    }
    void finish (bool completed = false)
    {
        if (!session)
            return;
        int result = -1;
        std::exception_ptr failure;
        try {
            result = aria2::shutdown (session, true);
            if (result >= 0) {
                do {
                    result = aria2::run (session, aria2::RUN_ONCE);
                    if (result > 0 && !failure) {
                        try {
                            progress (task, task->lastTotal, task->lastCompleted, task->lastSpeed);
                        } catch (...) {
                            failure = std::current_exception ();
                        }
                    }
                } while (result > 0);
            }
        } catch (...) {
            failure = std::current_exception ();
        }
        aria2::Session* finished = session;
        session = NULL;
        int finalResult = aria2::sessionFinal (finished);
        if (failure)
            std::rethrow_exception (failure);
        // IN_PROGRESS (7) is normal when shutting down for pause/cancel.
        require (result >= 0 && (!completed || finalResult == 0));
    }
};

typedef std::unique_ptr<aria2::DownloadHandle, void (*) (aria2::DownloadHandle*)> Handle;

static Handle handle (aria2::Session* session, aria2::A2Gid gid)
{
    Handle result (aria2::getDownloadHandle (session, gid), aria2::deleteDownloadHandle);
    require (result.get () != NULL);
    return result;
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

struct DownloadNode
{
    aria2::A2Gid gid;
    bool checked;
    bool metadata;
    explicit DownloadNode (aria2::A2Gid value) : gid (value), checked (false), metadata (false) {}
};

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
                            std::vector<DownloadNode>& nodes, const std::string& base)
{
    aria2::KeyVals options = { { "pause", "true" } };
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

static int execute (ProtocolTask* task)
{
    Runtime& shared = runtime ();
    SessionOwner owner (shared, task);
    while (!c_mutex_try_lock (&shared.mutex)) {
        progress (task, task->source.totalBytes, task->source.downloadedBytes, 0);
        if (request (task) != DOWNLOAD_ARIA_REQUEST_RUN)
            return request (task) == DOWNLOAD_ARIA_REQUEST_PAUSE ? 1 : -1;
        struct timespec delay = { 0, 100000000 };
        while (nanosleep (&delay, &delay) != 0)
            require (errno == EINTR, errno);
    }
    owner.locked = true;
    progress (task, task->source.totalBytes, task->source.downloadedBytes, 0);
    if (request (task) != DOWNLOAD_ARIA_REQUEST_RUN)
        return request (task) == DOWNLOAD_ARIA_REQUEST_PAUSE ? 1 : -1;
    if (!shared.initialized) {
        require (aria2::libraryInit () == 0);
        shared.initialized = true;
    }

    std::unique_ptr<char, void (*) (void*)> canonical (
        c_path_canonicalize (task->source.tempPath, NULL), c_free);
    require (canonical.get () != NULL, errno);
    std::string stage = canonical.get ();
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
    std::string privateDirectory = stage + "/.downloader-aria";
    if (c_directory_create_with_mode (privateDirectory.c_str (), 0700) != 0)
        require (errno == EEXIST, errno);
    aria2::KeyVals options = {
        { "no-conf", "true" }, { "no-netrc", "true" }, { "enable-rpc", "false" },
        { "check-certificate", "true" }, { "dir", stage }, { "continue", "true" },
        { "auto-file-renaming", "false" }, { "allow-overwrite", "false" },
        { "check-integrity", "true" }, { "seed-time", "0" }, { "seed-ratio", "0.0" },
        { "follow-torrent", "mem" }, { "follow-metalink", "mem" }, { "pause-metadata", "true" },
        { "bt-load-saved-metadata", "false" },
        { "bt-save-metadata", "false" }, { "bt-enable-lpd", "false" },
        { "enable-dht", "true" }, { "enable-dht6", "false" },
        { "dht-file-path", privateDirectory + "/dht" }, { "dht-file-path6", privateDirectory + "/dht6" },
        { "max-download-result", "2147483647" }, { "auto-save-interval", "1" },
        { "file-allocation", "none" }, { "console-log-level", "error" },
        { "summary-interval", "0" }, { "download-result", "hide" }
    };
    aria2::SessionConfig config;
    config.useSignalHandler = false;
    config.keepRunning = true;
    owner.session = aria2::sessionNew (options, config);
    require (owner.session != NULL);
    aria2::Session* session = owner.session;
    std::string url = task->source.resourceUrl;
    bool magnet = url.compare (0, 7, "magnet:") == 0;
    bool metalink = strcmp (task->source.protocol, "metalink") == 0;
    std::string announced;
    std::string fetched = privateDirectory + "/descriptor";
    std::vector<DownloadNode> nodes;
    aria2::A2Gid descriptor = 0;
    bool fetching = false;
    if (magnet) {
        aria2::A2Gid gid = 0;
        require (aria2::addUri (session, &gid, { url }, { { "pause", "true" } }) == 0, EINVAL);
        require (!aria2::isNull (gid), EINVAL);
        nodes.push_back (DownloadNode (gid));
        nodes.back ().metadata = true;
    } else if (url.compare (0, 7, "file://") == 0) {
        add_descriptor (session, local_metadata (url), metalink, nodes, "");
    } else {
        aria2::KeyVals fetchOptions = {
            { "dir", privateDirectory }, { "out", "descriptor" },
            { "follow-torrent", "false" }, { "follow-metalink", "false" },
            { "enable-http-pipelining", "false" }
        };
        require (aria2::addUri (session, &descriptor, { url }, fetchOptions) == 0, EINVAL);
        require (!aria2::isNull (descriptor), EINVAL);
        fetching = true;
    }
    if (task->callbacks.onStarted)
        task->callbacks.onStarted (task->userData);
    std::set<std::string> metadataFiles;
    std::set<std::string> payloadFiles;
    for (;;) {
        if (request (task) != DOWNLOAD_ARIA_REQUEST_RUN) {
            owner.finish ();
            return request (task) == DOWNLOAD_ARIA_REQUEST_PAUSE ? 1 : -1;
        }
        if (fetching) {
            Handle download = handle (session, descriptor);
            require (download->getStatus () != aria2::DOWNLOAD_ERROR, EIO);
            if (download->getStatus () == aria2::DOWNLOAD_COMPLETE) {
                download.reset ();
                add_descriptor (session, fetched, metalink, nodes, url);
                fetching = false;
            }
        }
        uint64_t total = 0, completed = 0, speed = 0;
        bool done = !fetching && !nodes.empty ();
        size_t payloadCount = 0;
        payloadFiles.clear ();
        std::set<std::string> controlFiles;
        for (size_t index = 0; index < nodes.size (); ++index) {
            Handle download = handle (session, nodes[index].gid);
            std::vector<aria2::A2Gid> children = download->getFollowedBy ();
            // Metalink metaurl torrents are auxiliary metadata with belongsTo,
            // rather than followedBy. Their parent is the actual payload task.
            aria2::A2Gid parent = download->getBelongsTo ();
            bool auxiliary = !aria2::isNull (parent);
            if (auxiliary) {
                nodes[index].metadata = true;
                append_nodes (nodes, { parent });
            }
            if (!children.empty ()) {
                nodes[index].metadata = true;
                append_nodes (nodes, children);
            }
            // Post-download parse errors can be swallowed by libaria2 while
            // leaving the descriptor status COMPLETE. A recognized descriptor
            // is never a terminal payload, even if no children were generated.
            if (download->getInfoHash ().empty ()) {
                for (const aria2::FileData& file : download->getFiles ()) {
                    if (descriptor_path (file.path))
                        nodes[index].metadata = true;
                }
            }
            aria2::DownloadStatus status = download->getStatus ();
            require (status != aria2::DOWNLOAD_ERROR && status != aria2::DOWNLOAD_REMOVED, EIO);
            if (!nodes[index].checked) {
                if (!nodes[index].metadata)
                    validate_files (download.get (), stage);
                if (status == aria2::DOWNLOAD_PAUSED)
                    require (aria2::unpauseDownload (session, nodes[index].gid) == 0);
                nodes[index].checked = true;
            }
            if (status != aria2::DOWNLOAD_COMPLETE)
                done = false;
            if (nodes[index].metadata) {
                // Prefer memory, but aria2 may keep known-length followed
                // descriptors on disk. Track only exact paths reported by a
                // metadata node, never infer ownership from a file suffix.
                for (const aria2::FileData& file : download->getFiles ()) {
                    if (file.path.compare (0, stage.size () + 1, stage + "/") == 0) {
                        require (safe_relative (file.path.substr (stage.size () + 1)), EPERM);
                        metadataFiles.insert (file.path);
                    }
                }
                if (status == aria2::DOWNLOAD_COMPLETE)
                    require (auxiliary || !children.empty (), EINVAL);
                continue;
            }
            ++payloadCount;
            for (const aria2::FileData& file : download->getFiles ()) {
                if (!file.path.empty ()) {
                    payloadFiles.insert (file.path);
                    // Multi-file torrents use <torrent-root>.aria2, outside
                    // their nested payload directory but still inside stage.
                    size_t slash = file.path.find ('/', stage.size () + 1);
                    if (slash != std::string::npos && !download->getInfoHash ().empty ())
                        controlFiles.insert (file.path.substr (0, slash) + ".aria2");
                    else
                        controlFiles.insert (file.path + ".aria2");
                }
            }
            total += static_cast<uint64_t> (std::max<int64_t> (0, download->getTotalLength ()));
            completed += static_cast<uint64_t> (std::max<int64_t> (0, download->getCompletedLength ()));
            speed += static_cast<uint64_t> (std::max (0, download->getDownloadSpeed ()));
            std::string name;
            if (status == aria2::DOWNLOAD_ACTIVE || status == aria2::DOWNLOAD_WAITING || status == aria2::DOWNLOAD_PAUSED)
                name = download->getBtMetaInfo ().name;
            bool torrentName = !name.empty ();
            if (name.empty ())
                name = descriptor_name (url);
            name = safe_name (name);
            if (announced.empty () || (torrentName && name != announced)) {
                announced = name;
                if (task->callbacks.onFileName)
                    task->callbacks.onFileName (task->userData, name.c_str ());
            }
        }
        for (const std::string& control : controlFiles)
            require (payloadFiles.count (control) == 0, EINVAL);
        progress (task, total, completed, speed);
        if (request (task) != DOWNLOAD_ARIA_REQUEST_RUN)
            continue;
        if (done) {
            require (payloadCount > 0, EINVAL);
            owner.finish (true);
            if (request (task) != DOWNLOAD_ARIA_REQUEST_RUN)
                return request (task) == DOWNLOAD_ARIA_REQUEST_PAUSE ? 1 : -1;
            check_tree (stage);
            require (!payloadFiles.empty (), EINVAL);
            for (const std::string& path : payloadFiles) {
                struct stat status;
                require_system (lstat (path.c_str (), &status));
                require (S_ISREG (status.st_mode) && status.st_nlink == 1, EINVAL);
            }
            for (const std::string& path : metadataFiles) {
                require (payloadFiles.count (path) == 0, EINVAL);
                remove_owned (path);
                // A later child can legitimately be named descriptor.aria2.
                if (payloadFiles.count (path + ".aria2") == 0)
                    remove_owned (path + ".aria2");
            }
            if (!magnet && url.compare (0, 7, "file://") != 0) {
                remove_owned (fetched);
                remove_owned (fetched + ".aria2");
            }
            remove_owned (privateDirectory + "/dht");
            remove_owned (privateDirectory + "/dht6");
            require_system (c_directory_remove (privateDirectory.c_str ()));
            // aria2 itself removes successful payload control files. Never glob
            // *.aria2: that suffix may be the user's actual payload filename.
            return 0;
        }
        require (aria2::run (session, aria2::RUN_ONCE) >= 0);
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
    ProtocolTask* task = static_cast<ProtocolTask*> (c_malloc0 (sizeof (*task)));
    if (!task) {
        errno = ENOMEM;
        return -1;
    }
    if (!c_mutex_init (&task->mutex)) {
        c_free (task);
        errno = ENOMEM;
        return -1;
    }
    task->source = *source;
    if (callbacks)
        task->callbacks = *callbacks;
    task->userData = userData;
    task->request = DOWNLOAD_ARIA_REQUEST_RUN;
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
    if (!task) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&task->mutex);
    if (task->request != DOWNLOAD_ARIA_REQUEST_CANCEL)
        task->request = value;
    c_mutex_unlock (&task->mutex);
    return 0;
}

static int aria_pause (ProtocolTask* task) { return set_request (task, DOWNLOAD_ARIA_REQUEST_PAUSE); }
static int aria_resume (ProtocolTask* task) { return set_request (task, DOWNLOAD_ARIA_REQUEST_RUN); }
static int aria_cancel (ProtocolTask* task) { return set_request (task, DOWNLOAD_ARIA_REQUEST_CANCEL); }

static void aria_close (ProtocolTask* task)
{
    if (task) {
        c_mutex_clear (&task->mutex);
        c_free (task);
    }
}
} // namespace

extern "C" const ProtocolOps* aria_protocol_ops (void)
{
    static const ProtocolOps operations = {
        "aria2", aria_can_handle, aria_open, aria_run, aria_pause, aria_resume, aria_cancel, aria_close
    };
    return &operations;
}
