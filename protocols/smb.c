#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "smb.h"

#include <clib/json.h>
#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/uri.h>
#include <time.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DOWNLOADER_SMB_CHUNK 65536U
#define DOWNLOADER_SMB_TIMEOUT_MS 30000U
/* Public setter accepts int; bundled libsmb2 exposes the enum only privately. */
#define DOWNLOADER_SMB_AUTH_NTLMSSP 1
#define DOWNLOADER_SMB_AUTH_KRB5    2

typedef struct
{
    CMutex          mutex;
    unsigned        references;
    bool            done;
    int             error;
    struct gaicb    request;
    struct addrinfo hints;
    char            host[DOWNLOAD_TASK_URL_SIZE];
    char            address[NI_MAXHOST];
} SmbResolver;

struct ProtocolTask
{
    ProtocolCallbacks callbacks;
    void*             userData;
    char*             host;
    char*             user;
    char*             password;
    char*             share;
    char*             path;
    char              tempPath[DOWNLOAD_TASK_PATH_SIZE];
    unsigned          port;
    bool              pauseRequested;
    bool              cancelRequested;
    bool              kerberos;
    char*             domain;
    bool              pending;
    int               status;
    void*             reply;
    struct smb2_context* smb;
    struct smb2fh*    handle;
    FILE*             file;
    uint64_t          total;
    uint64_t          downloaded;
    uint8_t           buffer[DOWNLOADER_SMB_CHUNK];
};

static uint64_t smb_now (void)
{
    struct timespec now;
    if (clock_gettime (CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

/* Called even before DNS/connect, and at most 100ms apart while waiting.
 * Reentrant pause/cancel from the core callback needs no cross-thread lock. */
static int smb_progress (ProtocolTask* task)
{
    if (task->callbacks.onProgress != NULL)
        task->callbacks.onProgress (task->userData, task->total, task->downloaded, 0);
    if (task->cancelRequested || task->pauseRequested) {
        errno = ECANCELED;
        return -1;
    }
    return 0;
}

static void smb_resolver_release (SmbResolver* resolver)
{
    bool destroy;
    c_mutex_lock (&resolver->mutex);
    destroy = --resolver->references == 0;
    c_mutex_unlock (&resolver->mutex);
    if (destroy) {
        if (resolver->request.ar_result != NULL)
            freeaddrinfo (resolver->request.ar_result);
        c_mutex_clear (&resolver->mutex);
        c_free (resolver);
    }
}

/* The notification retains the request after a timeout/cancel. No task or
 * credentials are retained, and completion releases all resolver resources. */
static void smb_resolved (union sigval value)
{
    SmbResolver* resolver = value.sival_ptr;
    int result = gai_error (&resolver->request);
    int error = EHOSTUNREACH;
    if (result == 0) {
        struct addrinfo* address = resolver->request.ar_result;
        if (address != NULL && getnameinfo (address->ai_addr, address->ai_addrlen,
                resolver->address, sizeof (resolver->address), NULL, 0, NI_NUMERICHOST) == 0)
            error = 0;
    } else if (result == EAI_MEMORY) {
        error = ENOMEM;
    } else if (result == EAI_AGAIN) {
        error = EAGAIN;
    }
    c_mutex_lock (&resolver->mutex);
    resolver->error = error;
    resolver->done = true;
    c_mutex_unlock (&resolver->mutex);
    smb_resolver_release (resolver);
}

static int smb_resolve (ProtocolTask* task, char* address, size_t capacity)
{
    SmbResolver* resolver = c_malloc0 (sizeof (*resolver));
    struct sigevent notification = {0};
    uint64_t started = smb_now ();
    int error = 0;
    if (resolver == NULL)
        return -1;
    if (!c_mutex_init (&resolver->mutex)) {
        c_free (resolver);
        return -1;
    }
    resolver->references = 2;
    snprintf (resolver->host, sizeof (resolver->host), "%s", task->host);
    resolver->hints.ai_socktype = SOCK_STREAM;
    resolver->hints.ai_family = AF_UNSPEC;
    resolver->request.ar_name = resolver->host;
    resolver->request.ar_request = &resolver->hints;
    notification.sigev_notify = SIGEV_THREAD;
    notification.sigev_notify_function = smb_resolved;
    notification.sigev_value.sival_ptr = resolver;
    struct gaicb* requests[] = {&resolver->request};
    if (getaddrinfo_a (GAI_NOWAIT, requests, 1, &notification) != 0) {
        smb_resolver_release (resolver);
        smb_resolver_release (resolver);
        errno = EHOSTUNREACH;
        return -1;
    }
    for (;;) {
        if (smb_progress (task) != 0) {
            error = errno;
            break;
        }
        c_mutex_lock (&resolver->mutex);
        bool done = resolver->done;
        error = resolver->error;
        if (done && error == 0)
            snprintf (address, capacity, "%s", resolver->address);
        c_mutex_unlock (&resolver->mutex);
        if (done)
            break;
        if (smb_now () - started >= DOWNLOADER_SMB_TIMEOUT_MS) {
            error = ETIMEDOUT;
            break;
        }
        if (poll (NULL, 0, 100) < 0 && errno != EINTR) {
            error = errno;
            break;
        }
    }
    smb_resolver_release (resolver);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static void smb_finished (struct smb2_context* smb, int status, void* reply, void* userData)
{
    ProtocolTask* task = userData;
    if (status < 0) {
        switch ((uint32_t)smb2_get_nterror (smb)) {
        case SMB2_STATUS_FILE_IS_A_DIRECTORY:
            status = -EISDIR;
            break;
        case SMB2_STATUS_LOGON_FAILURE:
        case SMB2_STATUS_WRONG_PASSWORD:
        case SMB2_STATUS_PASSWORD_MUST_CHANGE:
            status = -EACCES;
            break;
        default:
            break;
        }
    }
    task->status = status;
    task->reply = reply;
    task->pending = false;
}

/* Every request is sequential. Its callback and buffers remain valid through
 * destroy_context, which aborts any outstanding command before freeing it. */
static int smb_wait (ProtocolTask* task, int submitted)
{
    uint64_t started = smb_now ();
    if (submitted < 0) {
        task->pending = false;
        errno = -submitted;
        return -1;
    }
    while (task->pending) {
        struct pollfd socket;
        if (smb_progress (task) != 0)
            return -1;
        if (smb_now () - started >= DOWNLOADER_SMB_TIMEOUT_MS) {
            errno = ETIMEDOUT;
            return -1;
        }
        socket.fd = smb2_get_fd (task->smb);
        socket.events = (short)smb2_which_events (task->smb);
        socket.revents = 0;
        int result = poll (&socket, 1, 100);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (smb2_service (task->smb, socket.revents) < 0) {
            errno = task->status < 0 ? -task->status : ECONNRESET;
            return -1;
        }
    }
    if (task->status < 0) {
        errno = -task->status;
        return -1;
    }
    return smb_progress (task);
}

static void smb_prepare (ProtocolTask* task)
{
    task->pending = true;
    task->status = 0;
    task->reply = NULL;
}

static int smb_remote_stat (ProtocolTask* task, struct smb2_stat_64* attributes)
{
    smb_prepare (task);
    return smb_wait (task, smb2_fstat_async (task->smb, task->handle, attributes, smb_finished, task));
}

static int smb_read (ProtocolTask* task, uint64_t offset, uint32_t count)
{
    uint32_t maximum = smb2_get_max_read_size (task->smb);
    if (maximum == 0) {
        errno = EIO;
        return -1;
    }
    if (count > maximum)
        count = maximum;
    smb_prepare (task);
    if (smb_wait (task, smb2_pread_async (task->smb, task->handle, task->buffer, count,
                                        offset, smb_finished, task)) != 0)
        return -1;
    if (task->status == 0 || (uint32_t)task->status > count) {
        errno = ESTALE;
        return -1;
    }
    return task->status;
}

static int smb_decode (CStringView encoded, char** decoded)
{
    size_t length;
    if (c_uri_unescape_string (encoded, decoded, &length) != 0)
        return -1;
    if (memchr (*decoded, '\0', length) != NULL) {
        c_free (*decoded);
        *decoded = NULL;
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static bool smb_can_handle (const char* url, const char* protocol)
{
    return url != NULL && protocol != NULL && strcmp (protocol, "smb") == 0 &&
           strncasecmp (url, "smb://", 6) == 0;
}

static void smb_close (ProtocolTask* task)
{
    int saved = errno;
    if (task == NULL)
        return;
    if (task->smb != NULL)
        smb2_destroy_context (task->smb);
    if (task->file != NULL)
        (void)fclose (task->file);
    c_free (task->host);
    c_free (task->user);
    if (task->password != NULL) {
        volatile char* password = task->password;
        for (size_t i = 0; task->password[i] != '\0'; ++i)
            password[i] = 0;
    }
    c_free (task->password);
    c_free (task->domain);
    c_free (task->share);
    c_free (task->path);
    c_free (task);
    errno = saved;
}

static int smb_auth_options (ProtocolTask* task, const char* extra)
{
    CJsonDocument* document;
    CJsonValue*    options;
    int            result = 0;

    if (extra == NULL || extra[0] == '\0')
        return 0;
    document = c_json_document_new_from_data (c_string_view_from_cstr (extra), C_JSON_PARSE_NONE, NULL);
    if (document == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (c_json_value_type (c_json_document_root (document)) != C_JSON_TYPE_OBJECT) {
        errno = EINVAL;
        result = -1;
        goto done;
    }
    options = c_json_object_get (c_json_document_root (document), c_string_view_from_cstr ("smb"));
    if (options != NULL) {
        CJsonValue* auth;
        CJsonValue* domain;
        if (c_json_value_type (options) != C_JSON_TYPE_OBJECT) {
            errno = EINVAL;
            result = -1;
            goto done;
        }
        auth = c_json_object_get (options, c_string_view_from_cstr ("auth"));
        if (auth != NULL) {
            CStringView value = c_json_value_get_string (auth);
            if (c_json_value_type (auth) != C_JSON_TYPE_STRING) {
                errno = EINVAL;
                result = -1;
                goto done;
            }
            if ((value.size == 8 && strncasecmp (value.data, "kerberos", 8) == 0) ||
                (value.size == 4 && strncasecmp (value.data, "krb5", 4) == 0)) {
                task->kerberos = true;
            } else if (!((value.size == 4 && strncasecmp (value.data, "ntlm", 4) == 0) ||
                         (value.size == 7 && strncasecmp (value.data, "ntlmssp", 7) == 0))) {
                errno = EINVAL;
                result = -1;
                goto done;
            }
        }
        domain = c_json_object_get (options, c_string_view_from_cstr ("domain"));
        if (domain != NULL) {
            CStringView value = c_json_value_get_string (domain);
            char*       copy;
            if (c_json_value_type (domain) != C_JSON_TYPE_STRING || value.size == 0 || value.size > 253 ||
                memchr (value.data, '\0', value.size) != NULL || memchr (value.data, '\r', value.size) != NULL ||
                memchr (value.data, '\n', value.size) != NULL) {
                errno = EINVAL;
                result = -1;
                goto done;
            }
            copy = (char*)c_malloc (value.size + 1);
            if (copy == NULL) {
                result = -1;
                goto done;
            }
            memcpy (copy, value.data, value.size);
            copy[value.size] = '\0';
            task->domain = copy;
        }
    }
done:
    c_json_document_free (document);
    return result;
}

static int smb_open (ProtocolTask** result, const DownloadTask* download,
                     const ProtocolCallbacks* callbacks, void* userData)
{
    ProtocolTask* task = NULL;
    CUriObject* uri = NULL;
    CUriAuthority authority;
    CStringView path;
    bool present;
    size_t errorOffset;
    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    if (download == NULL || memchr (download->resourceUrl, 0, sizeof (download->resourceUrl)) == NULL ||
        memchr (download->protocol, 0, sizeof (download->protocol)) == NULL ||
        memchr (download->tempPath, 0, sizeof (download->tempPath)) == NULL ||
        !smb_can_handle (download->resourceUrl, download->protocol) || download->tempPath[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    task = c_malloc0 (sizeof (*task));
    if (task == NULL)
        return -1;
    uri = c_uri_object_new (c_string_view_from_cstr (download->resourceUrl), &errorOffset);
    if (uri == NULL)
        goto invalid;
    const CUri* view;
    if (c_uri_object_get_view (uri, &view) != 0 || view->hasQuery || view->hasFragment ||
        c_uri_object_get_authority (uri, &authority) != 0 || authority.host.size == 0 ||
        (authority.hasPort && authority.port == 0))
        goto invalid;
    /* The clib authority view preserves IPv6 brackets. */
    CStringView host = authority.host;
    if (host.size > 1 && host.data[0] == '[' && host.data[host.size - 1] == ']') {
        ++host.data;
        host.size -= 2;
    }
    if (smb_decode (host, &task->host) != 0)
        goto invalid;
    task->port = authority.hasPort ? authority.port : 445;
    CStringView user = c_string_view_from_cstr ("");
    CStringView password = user;
    if (authority.hasUserInfo) {
        user = authority.userInfo;
        const char* separator = memchr (user.data, ':', user.size);
        if (separator != NULL) {
            password = c_string_view_from_data (separator + 1, user.size - (size_t)(separator + 1 - user.data));
            user.size = (size_t)(separator - user.data);
        }
    }
    if (smb_decode (user, &task->user) != 0 || smb_decode (password, &task->password) != 0 ||
        c_uri_object_get_path (uri, &path, &present) != 0 || !present || path.size < 4 || path.data[0] != '/')
        goto invalid;
    const char* slash = memchr (path.data + 1, '/', path.size - 1);
    if (slash == NULL || slash == path.data + 1 || slash == path.data + path.size - 1)
        goto invalid;
    if (smb_decode (c_string_view_from_data (path.data + 1, (size_t)(slash - path.data - 1)), &task->share) != 0 ||
        smb_decode (c_string_view_from_data (slash + 1, path.size - (size_t)(slash + 1 - path.data)), &task->path) != 0)
        goto invalid;
    if (strpbrk (task->share, "/\\") != NULL || task->host[0] == '\0' ||
        strpbrk (task->host, "/\\@ \t\r\n") != NULL)
        goto invalid;
    if (smb_auth_options (task, download->extraJson) != 0)
        goto invalid;
    if (callbacks != NULL)
        task->callbacks = *callbacks;
    task->userData = userData;
    memcpy (task->tempPath, download->tempPath, sizeof (task->tempPath));
    c_uri_object_free (uri);
    *result = task;
    return 0;
invalid:
    c_uri_object_free (uri);
    smb_close (task);
    errno = EINVAL;
    return -1;
}

static bool smb_same_file (const struct smb2_stat_64* before, const struct smb2_stat_64* after)
{
    return before->smb2_size == after->smb2_size && before->smb2_ino == after->smb2_ino &&
           before->smb2_mtime == after->smb2_mtime && before->smb2_mtime_nsec == after->smb2_mtime_nsec &&
           before->smb2_ctime == after->smb2_ctime && before->smb2_ctime_nsec == after->smb2_ctime_nsec;
}

static int smb_transfer (ProtocolTask* task)
{
    char address[NI_MAXHOST];
    char server[NI_MAXHOST + 16];
    struct smb2_stat_64 before = {0};
    struct smb2_stat_64 after = {0};
    struct stat local = {0};
    if (smb_progress (task) != 0 || smb_resolve (task, address, sizeof (address)) != 0)
        return -1;
    if (task->kerberos) {
        /* The Kerberos SPN is cifs/<host>; the bundled libsmb2 patch strips the
         * port before building it. Use the URL hostname (not the numeric
         * address) so the SPN matches the server keytab. */
        snprintf (server, sizeof (server), strchr (task->host, ':') != NULL ? "[%s]:%u" : "%s:%u",
                  task->host, task->port);
    } else {
        snprintf (server, sizeof (server), strchr (address, ':') != NULL ? "[%s]:%u" : "%s:%u", address,
                  task->port);
    }
    task->smb = smb2_init_context ();
    if (task->smb == NULL) {
        errno = ENOMEM;
        return -1;
    }
    smb2_set_version (task->smb, SMB2_VERSION_ANY);
    smb2_set_authentication (task->smb, task->kerberos ? DOWNLOADER_SMB_AUTH_KRB5
                                                       : DOWNLOADER_SMB_AUTH_NTLMSSP);
    smb2_set_user (task->smb, task->user);
    smb2_set_password (task->smb, task->user[0] == '\0' ? NULL : task->password);
    if (task->kerberos && task->domain != NULL)
        smb2_set_domain (task->smb, task->domain);
    if (task->kerberos && task->password[0] != '\0') {
        /* Use the password to acquire a ticket in an isolated MEMORY ccache
         * instead of the process-wide KRB5CCNAME. */
        if (task->domain == NULL) {
            errno = EINVAL;
            return -1;
        }
        smb2_set_use_cached_creds (task->smb, 1);
    }
    smb2_set_security_mode (task->smb, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout (task->smb, DOWNLOADER_SMB_TIMEOUT_MS / 1000);
    smb_prepare (task);
    if (smb_wait (task, smb2_connect_share_async (task->smb, server, task->share, NULL,
                                                smb_finished, task)) != 0)
        return -1;
    smb_prepare (task);
    if (smb_wait (task, smb2_open_async (task->smb, task->path, O_RDONLY, smb_finished, task)) != 0)
        return -1;
    task->handle = task->reply;
    if (smb_remote_stat (task, &before) != 0)
        return -1;
    if (before.smb2_type != SMB2_TYPE_FILE) {
        errno = before.smb2_type == SMB2_TYPE_DIRECTORY ? EISDIR : ENOTSUP;
        return -1;
    }
    if (before.smb2_size > INT64_MAX) {
        errno = EFBIG;
        return -1;
    }
    task->total = before.smb2_size;
    int descriptor = open (task->tempPath, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (descriptor < 0)
        return -1;
    int statResult = fstat (descriptor, &local);
    if (statResult != 0 || !S_ISREG (local.st_mode)) {
        int saved = statResult != 0 ? errno : EINVAL;
        (void)close (descriptor);
        errno = saved;
        return -1;
    }
    /* A unique staging inode must not alias another local file. Validate the
     * opened descriptor before fdopen, prefix checks, truncation or writes. */
    if (local.st_nlink != 1) {
        (void)close (descriptor);
        errno = EMLINK;
        return -1;
    }
    task->file = fdopen (descriptor, "r+b");
    if (task->file == NULL) {
        int saved = errno;
        (void)close (descriptor);
        errno = saved;
        return -1;
    }
    uint64_t existing = (uint64_t)local.st_size;
    bool restart = existing > task->total;
    /* No durable remote validator in the core ABI: verify the entire local
     * prefix. This also detects same-size replacements across daemon restarts. */
    uint8_t previous[DOWNLOADER_SMB_CHUNK];
    uint64_t checked = 0;
    while (!restart && checked < existing) {
        uint32_t count = existing - checked > sizeof (previous) ? sizeof (previous) : (uint32_t)(existing - checked);
        int received = smb_read (task, checked, count);
        if (received < 0)
            return -1;
        if (fread (previous, 1, (size_t)received, task->file) != (size_t)received) {
            errno = EIO;
            return -1;
        }
        restart = memcmp (previous, task->buffer, (size_t)received) != 0;
        checked += (uint64_t)received;
    }
    if (restart) {
        if (ftruncate (descriptor, 0) != 0)
            return -1;
        existing = 0;
    }
    if (fseeko (task->file, (off_t)existing, SEEK_SET) != 0)
        return -1;
    task->downloaded = existing;
    while (task->downloaded < task->total) {
        uint64_t remaining = task->total - task->downloaded;
        uint32_t count = remaining > sizeof (task->buffer) ? sizeof (task->buffer) : (uint32_t)remaining;
        int received = smb_read (task, task->downloaded, count);
        if (received < 0)
            return -1;
        errno = 0;
        if (fwrite (task->buffer, 1, (size_t)received, task->file) != (size_t)received) {
            if (errno == 0)
                errno = EIO;
            return -1;
        }
        task->downloaded += (uint64_t)received;
    }
    if (smb_remote_stat (task, &after) != 0)
        return -1;
    if (!smb_same_file (&before, &after)) {
        /* Discard mixed-generation contents, including stdio buffered bytes. */
        if (fflush (task->file) != 0 || ftruncate (descriptor, 0) != 0)
            return -1;
        task->downloaded = 0;
        errno = ESTALE;
        return -1;
    }
    if (fflush (task->file) != 0 || fsync (descriptor) != 0)
        return -1;
    smb_prepare (task);
    if (smb_wait (task, smb2_close_async (task->smb, task->handle, smb_finished, task)) != 0)
        return -1;
    task->handle = NULL;
    return 0;
}

static const char* smb_error_message (int error)
{
    switch (error) {
    case EACCES: return "SMB authentication or access denied";
    case ENOENT: return "SMB share or file not found";
    case EISDIR: return "SMB resource is a directory";
    case ETIMEDOUT: return "SMB operation timed out";
    case ECANCELED: return "SMB download cancelled";
    case ESTALE: return "SMB remote file changed during download";
    case ECONNREFUSED: return "SMB connection refused";
    case ECONNRESET: return "SMB connection lost";
    case EHOSTUNREACH: return "SMB host unavailable";
    default: return "SMB download failed";
    }
}

static int smb_run (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->downloaded = 0;
    task->total = 0;
    if (task->callbacks.onStarted != NULL)
        task->callbacks.onStarted (task->userData);
    int result = smb_transfer (task);
    int error = result == 0 ? 0 : errno;
    char detail[DOWNLOAD_TASK_ERROR_SIZE];
    detail[0] = '\0';
    if (task->smb != NULL)
        snprintf (detail, sizeof (detail), "%s", smb2_get_error (task->smb));
    /* Abort commands and release all handles/sockets without waiting for a
     * disconnect handshake. Task-owned read buffers remain alive throughout. */
    if (task->smb != NULL) {
        smb2_destroy_context (task->smb);
        task->smb = NULL;
        task->handle = NULL;
    }
    if (task->file != NULL) {
        if (fclose (task->file) != 0 && (error == 0 || error == ECANCELED))
            error = errno;
        task->file = NULL;
    }
    if (task->cancelRequested && (error == 0 || error == ECANCELED))
        error = ECANCELED;
    if (task->pauseRequested && !task->cancelRequested && error == ECANCELED) {
        if (task->callbacks.onPaused != NULL)
            task->callbacks.onPaused (task->userData);
        return 1;
    }
    if (error != 0) {
        if (task->callbacks.onFailed != NULL) {
            char message[DOWNLOAD_TASK_ERROR_SIZE];
            snprintf (message, sizeof (message), "%s%s%s", smb_error_message (error),
                      detail[0] != '\0' ? ": " : "", detail);
            task->callbacks.onFailed (task->userData, error, message);
        }
        errno = error;
        return -1;
    }
    if (task->callbacks.onCompleted != NULL)
        task->callbacks.onCompleted (task->userData);
    return 0;
}

static int smb_pause (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = true;
    return 0;
}

static int smb_resume (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = false;
    return 0;
}

static int smb_cancel (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->cancelRequested = true;
    return 0;
}

static bool smb_retryable (ProtocolTask* task, int errorCode, const char* message)
{
    (void)task;
    (void)message;
    return errorCode == EIO || errorCode == EAGAIN || errorCode == ETIMEDOUT || errorCode == ECONNRESET ||
           errorCode == ECONNREFUSED || errorCode == EHOSTUNREACH || errorCode == ENETUNREACH ||
           errorCode == ENETDOWN || errorCode == EPIPE || errorCode == ECONNABORTED;
}

const ProtocolOps* smb_protocol_ops (void)
{
    static const ProtocolOps operations = {
        .name = "smb",
        .canHandle = smb_can_handle,
        .open = smb_open,
        .run = smb_run,
        .pause = smb_pause,
        .resume = smb_resume,
        .cancel = smb_cancel, .retryable = smb_retryable,
        .close = smb_close
    };
    return &operations;
}
