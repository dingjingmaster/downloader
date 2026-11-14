#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "request-context.h"

#include <clib/directory.h>
#include <clib/json.h>
#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/once.h>
#include <clib/thread.h>
#include <clib/time.h>
#include <clib/uri.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DOWNLOADER_HTTP_CHECKPOINT_INTERVAL (1024U * 1024U)

/* All task callbacks and pause/cancel operations run on the owning worker. */
struct ProtocolTask
{
    CURL*                    curl;
    FILE*                    file;
    struct curl_slist*       headers;
    const ProtocolCallbacks* callbacks;
    void*                    userData;
    char                     resourceUrl[DOWNLOAD_TASK_URL_SIZE];
    char                     protocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char                     tempPath[DOWNLOAD_TASK_PATH_SIZE];
    char                     extraJson[DOWNLOAD_TASK_EXTRA_JSON_SIZE];
    const RequestContext*    requestContext;
    int                      connectTimeout;
    bool                     pauseRequested;
    bool                     cancelRequested;
    bool                     isHttp;
    bool                     accepted;
    bool                     rangeSeen;
    bool                     lengthSeen;
    bool                     badHeaders;
    bool                     encoded;
    bool                     redirected;
    bool                     authNegotiating;
    bool                     proxyNegotiating;
    bool                     resumeRejected;
    int                      localError;
    int                      maxRetries;
    int                      transferTimeout;
    int64_t                  started;
    long                     status;
    CURLcode                 lastResult;
    uint64_t                 initialBytes;
    uint64_t                 received;
    uint64_t                 durableBytes;
    uint64_t                 rangeStart;
    uint64_t                 rangeEnd;
    uint64_t                 rangeTotal;
    uint64_t                 contentLength;
    uint64_t                 knownTotal;
    unsigned                 retryAfter;
    uint32_t                 checkpointVersion;
    char                     etag[513];
    char                     modified[513];
    char                     responseEtag[513];
    char                     responseModified[513];
    char                     persistedUrl[DOWNLOAD_TASK_URL_SIZE];
    char                     persistedContext[DOWNLOAD_TASK_CONTEXT_SIZE];
    char                     currentUrl[DOWNLOAD_TASK_URL_SIZE];
    char                     contextId[DOWNLOAD_TASK_CONTEXT_SIZE];
    char                     filename[1024];
    char                     extendedFilename[1024];
};

static COnce gsCurlOnce = C_ONCE_INIT;
static CURLcode gsCurlInit;

static void http_global_init (void* unused)
{
    (void)unused;
    gsCurlInit = curl_global_init (CURL_GLOBAL_DEFAULT);
}

static bool http_can_handle (const char* url, const char* protocol)
{
    (void)url;
    return protocol && (!strcmp (protocol, "http") || !strcmp (protocol, "https") ||
                        !strcmp (protocol, "ftp") || !strcmp (protocol, "ftps") || !strcmp (protocol, "sftp"));
}

static bool http_safe_value (const char* value, size_t length)
{
    return length <= 512 && !memchr (value, 0, length) &&
           !memchr (value, '\r', length) && !memchr (value, '\n', length);
}

static bool http_strong_etag (const char* value)
{
    size_t length = strlen (value);
    return length >= 2 && value[0] == '"' && value[length - 1] == '"';
}

static const char* http_validator (ProtocolTask* task)
{
    if (http_strong_etag (task->etag))
        return task->etag;
    return task->modified[0] && curl_getdate (task->modified, NULL) != (time_t)-1 ? task->modified : NULL;
}

static int http_flush (ProtocolTask* task)
{
    if (fflush (task->file) != 0) {
        task->localError = errno ? errno : EIO;
        return -1;
    }
    return 0;
}

static int http_reset_file (ProtocolTask* task)
{
    if (http_flush (task) != 0)
        return -1;
    if (ftruncate (fileno (task->file), 0) != 0 || fseeko (task->file, 0, SEEK_SET) != 0 ||
        fsync (fileno (task->file)) != 0) {
        task->localError = errno ? errno : EIO;
        return -1;
    }
    task->initialBytes = 0;
    task->received = 0;
    task->durableBytes = 0;
    task->knownTotal = 0;
    return 0;
}

static bool http_terminated (const char* value, size_t capacity)
{
    return memchr (value, '\0', capacity) != NULL;
}

/* The request context identifies everything that can change the representation
 * without changing the final URL. It contains no credentials. */
static void http_context_id (const char* extraJson, char* destination, size_t capacity)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char* byte = (const unsigned char*)extraJson; *byte; byte++) {
        hash ^= *byte;
        hash *= UINT64_C(1099511628211);
    }
    snprintf (destination, capacity, "fnv1a64:%016" PRIx64, hash);
}

static int http_effective_url (ProtocolTask* task)
{
    char* url = NULL;

    if (curl_easy_getinfo (task->curl, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || url == NULL)
        return -1;
    if (strlen (url) >= sizeof (task->currentUrl)) {
        /* The identity cannot be persisted or compared, but that must not fail
         * an otherwise valid download. Without it no checkpoint is written. */
        task->currentUrl[0] = '\0';
        return 0;
    }
    snprintf (task->currentUrl, sizeof (task->currentUrl), "%s", url);
    return 0;
}

/* Fsync precedes the callback: the core may only persist bytes that survive a
 * crash. Failure aborts the transfer rather than continuing uncheckpointed. */
static int http_checkpoint (ProtocolTask* task)
{
    DownloadTaskCheckpoint checkpoint;

    if (!task->isHttp || task->file == NULL || task->localError)
        return 0;
    if (task->callbacks == NULL || task->callbacks->onCheckpoint == NULL)
        return 0;
    /* No 2xx response has established a resource identity yet. Durable bytes
     * here belong to the already-persisted checkpoint or to the legacy
     * file-size path; writing an empty URL would erase a valid identity. */
    if (task->currentUrl[0] == '\0')
        return 0;
    memset (&checkpoint, 0, sizeof (checkpoint));
    checkpoint.version = DOWNLOAD_TASK_CHECKPOINT_VERSION;
    checkpoint.durableBytes = task->initialBytes + task->received;
    snprintf (checkpoint.effectiveUrl, sizeof (checkpoint.effectiveUrl), "%s", task->currentUrl);
    snprintf (checkpoint.etag, sizeof (checkpoint.etag), "%s", task->etag);
    snprintf (checkpoint.lastModified, sizeof (checkpoint.lastModified), "%s", task->modified);
    snprintf (checkpoint.requestContext, sizeof (checkpoint.requestContext), "%s", task->contextId);
    if (http_flush (task) != 0 || fsync (fileno (task->file)) != 0) {
        task->localError = errno ? errno : EIO;
        return -1;
    }
    task->durableBytes = checkpoint.durableBytes;
    if (task->callbacks->onCheckpoint (task->userData, &checkpoint) != 0) {
        task->localError = errno ? errno : ECANCELED;
        return -1;
    }
    return 0;
}

/* Same final URL and same request context prove the persisted prefix belongs to
 * the resource now being downloaded. Different URLs sharing an ETag prove
 * nothing. Legacy tasks have no persisted URL, so only a direct response is
 * accepted. */
static bool http_resume_identity_matches (ProtocolTask* task)
{
    if (task->checkpointVersion == DOWNLOAD_TASK_CHECKPOINT_VERSION) {
        return task->persistedUrl[0] != '\0' && task->persistedContext[0] != '\0' &&
               strcmp (task->persistedUrl, task->currentUrl) == 0 &&
               strcmp (task->persistedContext, task->contextId) == 0;
    }
    return !task->redirected;
}

static int http_progress (void* userData, curl_off_t total, curl_off_t downloaded,
                          curl_off_t uploadTotal, curl_off_t uploaded)
{
    ProtocolTask* task = userData;
    int64_t now;
    curl_off_t speed = 0;
    (void)downloaded;
    (void)uploadTotal;
    (void)uploaded;
    if (task->pauseRequested || task->cancelRequested || task->localError)
        return 1;
    if (c_time_monotonic_us (&now) != 0) {
        task->localError = errno;
        return 1;
    }
    if (task->transferTimeout > 0 && now - task->started >= (int64_t)task->transferTimeout * 1000000) {
        task->localError = ETIMEDOUT;
        return 1;
    }
    if (curl_easy_getinfo (task->curl, CURLINFO_SPEED_DOWNLOAD_T, &speed) != CURLE_OK)
        speed = 0;
    /* Poll/backoff callbacks supply zero, not a new unknown resource size.
     * Ignore redirect/error-body lengths reported by libcurl. */
    if (!task->isHttp || task->accepted) {
        if (task->isHttp && task->status == 206)
            task->knownTotal = task->rangeTotal;
        else if (task->isHttp && task->lengthSeen)
            task->knownTotal = task->initialBytes + task->contentLength;
        else if (total > 0)
            task->knownTotal = task->initialBytes + (uint64_t)total;
    }
    if (task->callbacks && task->callbacks->onProgress)
        task->callbacks->onProgress (task->userData, task->knownTotal,
            task->initialBytes + task->received, speed > 0 ? (uint64_t)speed : 0);
    if (task->accepted && task->initialBytes + task->received - task->durableBytes >=
        DOWNLOADER_HTTP_CHECKPOINT_INTERVAL && http_checkpoint (task) != 0)
        return 1;
    return task->pauseRequested || task->cancelRequested || task->localError;
}

static bool http_number (const char** cursor, uint64_t* result)
{
    const char* position = *cursor;
    uint64_t number = 0;
    if (*position < '0' || *position > '9')
        return false;
    while (*position >= '0' && *position <= '9') {
        unsigned digit = (unsigned)(*position++ - '0');
        if (number > (UINT64_MAX - digit) / 10)
            return false;
        number = number * 10 + digit;
    }
    *cursor = position;
    *result = number;
    return true;
}

/* Parse parameters independently: filename* wins even when filename follows it. */
static void http_filename (ProtocolTask* task, char* value)
{
    char* position = strchr (value, ';');
    while (position && *position) {
        char* key = ++position;
        while (*key == ' ' || *key == '\t')
            key++;
        char* equals = strchr (key, '=');
        char* separator = strchr (key, ';');
        if (!equals || (separator && separator < equals)) {
            position = separator;
            continue;
        }
        char* keyEnd = equals;
        while (keyEnd > key && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t'))
            keyEnd--;
        char* name = equals + 1;
        while (*name == ' ' || *name == '\t')
            name++;
        char* end;
        if (*name == '"') {
            name++;
            end = strchr (name, '"');
            if (!end)
                return;
            position = strchr (end + 1, ';');
        } else {
            end = strchr (name, ';');
            position = end;
            if (!end)
                end = name + strlen (name);
            while (end > name && (end[-1] == ' ' || end[-1] == '\t'))
                end--;
        }
        size_t length = (size_t)(end - name);
        bool extended = keyEnd - key == 9 && !strncasecmp (key, "filename*", 9);
        bool plain = keyEnd - key == 8 && !strncasecmp (key, "filename", 8);
        if ((!extended && !plain) || length >= sizeof (task->filename))
            continue;
        char buffer[1024];
        memcpy (buffer, name, length);
        buffer[length] = 0;
        if (plain) {
            memcpy (task->filename, buffer, length + 1);
        } else if (!strncasecmp (buffer, "UTF-8'", 6)) {
            char* encoded = strchr (buffer + 6, '\'');
            char* decoded = NULL;
            size_t decodedSize = 0;
            if (encoded && c_uri_unescape_string (c_string_view_from_cstr (encoded + 1), &decoded, &decodedSize) == 0) {
                if (decodedSize < sizeof (task->extendedFilename) && !memchr (decoded, 0, decodedSize) &&
                    !memchr (decoded, '\r', decodedSize) && !memchr (decoded, '\n', decodedSize)) {
                    memcpy (task->extendedFilename, decoded, decodedSize);
                    task->extendedFilename[decodedSize] = 0;
                }
                c_free (decoded);
            }
        }
    }
}

static int http_accept_headers (ProtocolTask* task)
{
    /* Error bodies are not needed. End the attempt after its headers so a
     * streaming error page cannot indefinitely postpone retry or recovery.
     * Authentication challenges are the exception: libcurl must consume the
     * 401/407 and retry with credentials inside the same perform. */
    if (task->status >= 400) {
        if ((task->status == 401 && task->authNegotiating) ||
            (task->status == 407 && task->proxyNegotiating))
            return 0;
        return -1;
    }
    if (task->status < 200 || task->status >= 300)
        return 0;
    if (task->badHeaders || task->encoded) {
        task->localError = task->encoded ? ENOTSUP : EPROTO;
        return -1;
    }
    if (http_effective_url (task) != 0) {
        task->localError = EPROTO;
        return -1;
    }
    if (task->status == 206) {
        /* Even equal ETags at different URLs prove nothing: the saved prefix
         * must belong to the resource that answered this request. */
        if (task->initialBytes && !http_resume_identity_matches (task)) {
            task->resumeRejected = true;
            return -1;
        }
        if (!task->rangeSeen || task->rangeStart != task->initialBytes ||
            task->rangeEnd < task->rangeStart || task->rangeTotal <= task->rangeEnd ||
            task->rangeEnd != task->rangeTotal - 1 ||
            (task->lengthSeen && task->contentLength != task->rangeEnd - task->rangeStart + 1) ||
            (task->initialBytes && http_strong_etag (task->etag) && task->responseEtag[0] &&
             strcmp (task->etag, task->responseEtag)) ||
            (task->initialBytes && !http_strong_etag (task->etag) && task->modified[0] &&
             task->responseModified[0] && strcmp (task->modified, task->responseModified))) {
            task->localError = EPROTO;
            return -1;
        }
    } else {
        if (task->status != 200 && task->status != 204) {
            task->localError = EPROTO;
            return -1;
        }
        /* Durable truncation precedes persisting a replacement validator. */
        if (task->initialBytes && http_reset_file (task) != 0)
            return -1;
    }
    if (task->status != 206 || !task->initialBytes) {
        task->etag[0] = 0;
        task->modified[0] = 0;
    }
    if (task->responseEtag[0])
        strcpy (task->etag, task->responseEtag);
    if (task->responseModified[0])
        strcpy (task->modified, task->responseModified);
    /* Validators now travel with the final effective URL, so a redirected
     * download can still resume while a changed target cannot. */
    if (task->callbacks && task->callbacks->onCheckpoint) {
        if (http_checkpoint (task) != 0)
            return -1;
    } else if (task->callbacks && task->callbacks->onMetadata) {
        errno = 0;
        if (task->callbacks->onMetadata (task->userData, task->etag, task->modified) != 0) {
            task->localError = errno ? errno : ECANCELED;
            return -1;
        }
    }
    if (task->pauseRequested || task->cancelRequested)
        return -1;
    task->accepted = true;
    task->knownTotal = task->status == 206 ? task->rangeTotal :
        (task->lengthSeen ? task->contentLength : 0);
    const char* filename = task->extendedFilename[0] ? task->extendedFilename : task->filename;
    if (filename[0] && task->callbacks && task->callbacks->onFileName)
        task->callbacks->onFileName (task->userData, filename);
    return task->pauseRequested || task->cancelRequested ? -1 : 0;
}

static size_t http_header (char* header, size_t size, size_t count, void* userData)
{
    ProtocolTask* task = userData;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (!task->isHttp)
        return length;
    if (length >= 5 && !strncasecmp (header, "HTTP/", 5)) {
        char* space = memchr (header, ' ', length);
        if (!space || header + length - space < 4)
            return 0;
        /* A new status block after 3xx proves libcurl followed a redirect;
         * do not depend on getinfo timing inside a header callback. */
        if (task->status >= 300 && task->status < 400)
            task->redirected = true;
        task->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        task->accepted = false;
        task->rangeSeen = false;
        task->lengthSeen = false;
        task->badHeaders = false;
        task->encoded = false;
        task->retryAfter = 0;
        task->responseEtag[0] = 0;
        task->responseModified[0] = 0;
        task->filename[0] = 0;
        task->extendedFilename[0] = 0;
        return length;
    }
    /* Ignore trailers after accepting the final block. */
    if (task->accepted)
        return length;
    if ((length == 2 && !memcmp (header, "\r\n", 2)) || (length == 1 && header[0] == '\n'))
        return http_accept_headers (task) == 0 ? length : 0;
    if (task->status < 200 || (task->status >= 300 && task->status < 400))
        return length;
    char line[8192];
    if (length >= sizeof (line) || memchr (header, 0, length)) {
        task->badHeaders = true;
        return length;
    }
    memcpy (line, header, length);
    line[length] = 0;
    char* colon = strchr (line, ':');
    if (!colon)
        return length;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    char* end = colon + strlen (colon);
    while (end > colon && strchr (" \t\r\n", end[-1]))
        *--end = 0;
    if (!strcasecmp (line, "ETag") || !strcasecmp (line, "Last-Modified")) {
        char* target = !strcasecmp (line, "ETag") ? task->responseEtag : task->responseModified;
        if (!http_safe_value (colon, strlen (colon)) || (target[0] && strcmp (target, colon)))
            task->badHeaders = true;
        else
            strcpy (target, colon);
    } else if (!strcasecmp (line, "Content-Range")) {
        const char* cursor = colon;
        if (task->rangeSeen || strncmp (cursor, "bytes ", 6)) {
            task->badHeaders = true;
        } else {
            cursor += 6;
            if (!http_number (&cursor, &task->rangeStart) || *cursor++ != '-' ||
                !http_number (&cursor, &task->rangeEnd) || *cursor++ != '/' ||
                !http_number (&cursor, &task->rangeTotal) || *cursor)
                task->badHeaders = true;
        }
        task->rangeSeen = true;
    } else if (!strcasecmp (line, "Content-Length")) {
        const char* cursor = colon;
        uint64_t number;
        if (!http_number (&cursor, &number) || *cursor ||
            (task->lengthSeen && number != task->contentLength))
            task->badHeaders = true;
        else
            task->contentLength = number;
        task->lengthSeen = true;
    } else if (!strcasecmp (line, "Content-Encoding")) {
        if (strcasecmp (colon, "identity"))
            task->encoded = true;
    } else if (!strcasecmp (line, "Content-Disposition")) {
        http_filename (task, colon);
    } else if (!strcasecmp (line, "Retry-After")) {
        const char* cursor = colon;
        uint64_t seconds;
        if (http_number (&cursor, &seconds) && !*cursor) {
            task->retryAfter = seconds > 60 ? 60 : (unsigned)seconds;
        } else {
            time_t date = curl_getdate (colon, NULL);
            int64_t now;
            if (date != (time_t)-1 && c_time_real_us (&now) == 0 && date > now / 1000000) {
                int64_t delay = date - now / 1000000;
                task->retryAfter = delay > 60 ? 60 : (unsigned)delay;
            }
        }
    }
    return length;
}

static size_t http_write (const void* bytes, size_t size, size_t count, void* userData)
{
    ProtocolTask* task = userData;
    if (size && count > SIZE_MAX / size)
        return 0;
    size_t length = size * count;
    if (task->pauseRequested || task->cancelRequested || task->localError)
        return 0;
    if (task->isHttp && !task->accepted) {
        /* Redirect/error bodies are never file content. */
        return length;
    }
    if (task->isHttp && task->status == 206 &&
        (task->received > task->rangeEnd - task->rangeStart + 1 ||
         length > task->rangeEnd - task->rangeStart + 1 - task->received)) {
        task->localError = EPROTO;
        return 0;
    }
    size_t written = fwrite (bytes, 1, length, task->file);
    task->received += written;
    if (written != length)
        task->localError = errno ? errno : EIO;
    return written;
}

static void http_close (ProtocolTask* task)
{
    if (!task)
        return;
    int savedErrno = errno;
    if (task->file && fclose (task->file) != 0 && !savedErrno)
        savedErrno = errno;
    curl_slist_free_all (task->headers);
    if (task->curl)
        curl_easy_cleanup (task->curl);
    c_free (task);
    errno = savedErrno;
}

static int http_options (ProtocolTask* context, CURL* curl, const char* extra)
{
    (void)context;
    if (!extra[0])
        return 0;
    CJsonDocument* document = c_json_document_new_from_data (c_string_view_from_cstr (extra), C_JSON_PARSE_NONE, NULL);
    if (!document)
        return -1;
    CJsonValue* root = c_json_document_root (document);
    CJsonValue* options = c_json_object_get (root, c_string_view_from_cstr ("http"));
    int result = -1;
    if (c_json_value_type (root) != C_JSON_TYPE_OBJECT ||
        (options && c_json_value_type (options) != C_JSON_TYPE_OBJECT)) {
        errno = EINVAL;
        goto done;
    }
    const char* keys[] = { "user_agent", "referer" };
    CURLoption curlOptions[] = { CURLOPT_USERAGENT, CURLOPT_REFERER };
    for (size_t index = 0; options && index < 2; index++) {
        CJsonValue* value = c_json_object_get (options, c_string_view_from_cstr (keys[index]));
        if (!value)
            continue;
        CStringView string = c_json_value_get_string (value);
        if (c_json_value_type (value) != C_JSON_TYPE_STRING || !http_safe_value (string.data, string.size)) {
            errno = EINVAL;
            goto done;
        }
        char option[513];
        memcpy (option, string.data, string.size);
        option[string.size] = 0;
        if (curl_easy_setopt (curl, curlOptions[index], option) != CURLE_OK) {
            errno = EIO;
            goto done;
        }
    }
    {
        CJsonValue* cookies = options ? c_json_object_get (options, c_string_view_from_cstr ("cookies")) : NULL;
        if (cookies != NULL) {
            size_t count = c_json_array_size (cookies);
            if (c_json_value_type (cookies) != C_JSON_TYPE_ARRAY || count > 64) {
                errno = EINVAL;
                goto done;
            }
            for (size_t index = 0; index < count; index++) {
                CJsonValue*  item = c_json_array_get (cookies, index);
                CStringView  text = c_json_value_get_string (item);
                char         line[4096];
                if (c_json_value_type (item) != C_JSON_TYPE_STRING || text.size == 0 || text.size > 3000 ||
                    memchr (text.data, '\0', text.size) != NULL || memchr (text.data, '\r', text.size) != NULL ||
                    memchr (text.data, '\n', text.size) != NULL) {
                    errno = EINVAL;
                    goto done;
                }
                snprintf (line, sizeof (line), "Set-Cookie: %.*s", (int)text.size, text.data);
                if (curl_easy_setopt (curl, CURLOPT_COOKIELIST, line) != CURLE_OK) {
                    errno = EIO;
                    goto done;
                }
            }
        }
    }
    result = 0;
done:
    {
        int savedErrno = errno;
        c_json_document_free (document);
        errno = savedErrno;
    }
    return result;
}

/* Every option is checked; failures exit before a network attempt. */
#define DOWNLOADER_CURL_SET(task, option, value) \
    do { \
        CURLcode optionResult = curl_easy_setopt ((task)->curl, option, value); \
        if (optionResult != CURLE_OK) { \
            errno = optionResult == CURLE_OUT_OF_MEMORY ? ENOMEM : EIO; \
            goto failed; \
        } \
    } while (0)

/* Shared by the single-stream handle and the three parallel segment handles. */
static int http_apply_common (ProtocolTask* task, CURL* curl)
{
#define HTTP_COMMON_SET(option, value) \
    do { \
        CURLcode result = curl_easy_setopt (curl, option, value); \
        if (result != CURLE_OK) { \
            errno = result == CURLE_OUT_OF_MEMORY ? ENOMEM : EIO; \
            return -1; \
        } \
    } while (0)
    HTTP_COMMON_SET (CURLOPT_URL, task->resourceUrl);
    HTTP_COMMON_SET (CURLOPT_USERAGENT, "downloader/0.1.0");
    if (task->isHttp)
        HTTP_COMMON_SET (CURLOPT_COOKIEFILE, "");
    if (http_options (task, curl, task->extraJson) != 0)
        return -1;
    if (task->requestContext != NULL) {
        const RequestContext* policy = task->requestContext;
        if (request_context_has_auth (policy)) {
            DownloaderCredentialKind kind = request_context_auth_kind (policy);
            const char* username = request_context_auth_username (policy);
            const char* secret = request_context_auth_secret (policy);
            char        userpwd[DOWNLOADER_CREDENTIAL_USER_SIZE + DOWNLOADER_CREDENTIAL_SECRET_SIZE + 2];
            if (kind == DOWNLOADER_CREDENTIAL_COOKIE) {
                /* Session cookies belong to adapters that know the target API;
                 * never downgrade them to Basic credentials for another host. */
                errno = ENOTSUP;
                return -1;
            }
            if (strlen (username) + strlen (secret) + 2 > sizeof (userpwd)) {
                errno = EINVAL;
                return -1;
            }
            if (!task->isHttp) {
                if (kind == DOWNLOADER_CREDENTIAL_BEARER) {
                    errno = ENOTSUP;
                    return -1;
                }
                snprintf (userpwd, sizeof (userpwd), "%s:%s", username, secret);
                HTTP_COMMON_SET (CURLOPT_USERPWD, userpwd);
            } else if (kind == DOWNLOADER_CREDENTIAL_BEARER) {
#if LIBCURL_VERSION_NUM >= 0x073D00
                HTTP_COMMON_SET (CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
                HTTP_COMMON_SET (CURLOPT_XOAUTH2_BEARER, secret);
#else
                errno = ENOTSUP;
                return -1;
#endif
            } else {
                snprintf (userpwd, sizeof (userpwd), "%s:%s", username, secret);
                HTTP_COMMON_SET (CURLOPT_USERPWD, userpwd);
                HTTP_COMMON_SET (CURLOPT_HTTPAUTH,
                                 kind == DOWNLOADER_CREDENTIAL_DIGEST ? (long)CURLAUTH_DIGEST
                                                                      : (long)CURLAUTH_BASIC);
                task->authNegotiating = true;
            }
        }
        if (request_context_has_proxy (policy)) {
            HTTP_COMMON_SET (CURLOPT_PROXY, request_context_proxy_url (policy));
            /* A configured proxy is mandatory: ignore no_proxy entirely. */
            HTTP_COMMON_SET (CURLOPT_NOPROXY, "");
            if (request_context_proxy_username (policy)[0] != '\0') {
                char userpwd[DOWNLOADER_CREDENTIAL_USER_SIZE + DOWNLOADER_CREDENTIAL_SECRET_SIZE + 2];
                if (strlen (request_context_proxy_username (policy)) +
                        strlen (request_context_proxy_secret (policy)) + 2 > sizeof (userpwd)) {
                    errno = EINVAL;
                    return -1;
                }
                snprintf (userpwd, sizeof (userpwd), "%s:%s", request_context_proxy_username (policy),
                          request_context_proxy_secret (policy));
                HTTP_COMMON_SET (CURLOPT_PROXYUSERPWD, userpwd);
                HTTP_COMMON_SET (CURLOPT_PROXYAUTH, CURLAUTH_ANY);
                task->proxyNegotiating = true;
            }
        }
    }
    HTTP_COMMON_SET (CURLOPT_FOLLOWLOCATION, 1L);
    HTTP_COMMON_SET (CURLOPT_MAXREDIRS, 10L);
#if LIBCURL_VERSION_NUM >= 0x075500
    HTTP_COMMON_SET (CURLOPT_PROTOCOLS_STR, task->isHttp ? "http,https" : task->protocol);
    HTTP_COMMON_SET (CURLOPT_REDIR_PROTOCOLS_STR, task->isHttp ? "http,https" : task->protocol);
#else
    long protocols = task->isHttp ? CURLPROTO_HTTP | CURLPROTO_HTTPS :
        (!strcmp (task->protocol, "ftp") ? CURLPROTO_FTP : !strcmp (task->protocol, "ftps") ? CURLPROTO_FTPS : CURLPROTO_SFTP);
    HTTP_COMMON_SET (CURLOPT_PROTOCOLS, protocols);
    HTTP_COMMON_SET (CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
    HTTP_COMMON_SET (CURLOPT_SSL_VERIFYPEER, 1L);
    HTTP_COMMON_SET (CURLOPT_SSL_VERIFYHOST, 2L);
    HTTP_COMMON_SET (CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    HTTP_COMMON_SET (CURLOPT_NOSIGNAL, 1L);
    HTTP_COMMON_SET (CURLOPT_CONNECTTIMEOUT, (long)(task->connectTimeout > 0 ? task->connectTimeout : 30));
    HTTP_COMMON_SET (CURLOPT_LOW_SPEED_LIMIT, 1L);
    HTTP_COMMON_SET (CURLOPT_LOW_SPEED_TIME, 60L);
    HTTP_COMMON_SET (CURLOPT_ACCEPT_ENCODING, "identity");
    HTTP_COMMON_SET (CURLOPT_HTTP_CONTENT_DECODING, 0L);
#undef HTTP_COMMON_SET
    return 0;
}

static int http_open (ProtocolTask** result, const DownloadTask* task,
                      const ProtocolCallbacks* callbacks, void* userData)
{
    if (!result || !task || !http_can_handle (task->resourceUrl, task->protocol) || !task->tempPath[0]) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    c_once_call (&gsCurlOnce, http_global_init, NULL);
    if (gsCurlInit != CURLE_OK) {
        errno = EIO;
        return -1;
    }
    const curl_version_info_data* version = curl_version_info (CURLVERSION_NOW);
    bool supported = false;
    for (const char* const* protocol = version ? version->protocols : NULL; protocol && *protocol; protocol++) {
        if (!strcmp (*protocol, task->protocol))
            supported = true;
    }
    if (!supported) {
        if (callbacks && callbacks->onFailed)
            callbacks->onFailed (userData, ENOTSUP, "Runtime libcurl does not support the requested protocol (errno ENOTSUP)");
        errno = ENOTSUP;
        return -1;
    }
    ProtocolTask* context = c_malloc0 (sizeof (*context));
    if (!context)
        return -1;
    context->callbacks = callbacks;
    context->userData = userData;
    context->maxRetries = task->maxRetries > 0 ? task->maxRetries : 0;
    context->transferTimeout = task->transferTimeout;
    context->knownTotal = task->totalBytes;
    context->isHttp = !strcmp (task->protocol, "http") || !strcmp (task->protocol, "https");
    snprintf (context->resourceUrl, sizeof (context->resourceUrl), "%s", task->resourceUrl);
    snprintf (context->protocol, sizeof (context->protocol), "%s", task->protocol);
    snprintf (context->extraJson, sizeof (context->extraJson), "%s", task->extraJson);
    snprintf (context->tempPath, sizeof (context->tempPath), "%s", task->tempPath);
    context->requestContext = task->requestContext;
    context->connectTimeout = task->connectTimeout;
    context->curl = curl_easy_init ();
    if (!context->curl) {
        errno = ENOMEM;
        goto failed;
    }
    if (!http_safe_value (task->etag, strlen (task->etag)) ||
        !http_safe_value (task->lastModified, strlen (task->lastModified))) {
        errno = EINVAL;
        goto failed;
    }
    strcpy (context->etag, task->etag);
    strcpy (context->modified, task->lastModified);
    if (task->checkpoint.version == DOWNLOAD_TASK_CHECKPOINT_VERSION &&
        http_terminated (task->checkpoint.effectiveUrl, sizeof (task->checkpoint.effectiveUrl)) &&
        http_terminated (task->checkpoint.requestContext, sizeof (task->checkpoint.requestContext))) {
        context->checkpointVersion = task->checkpoint.version;
        snprintf (context->persistedUrl, sizeof (context->persistedUrl), "%s", task->checkpoint.effectiveUrl);
        snprintf (context->persistedContext, sizeof (context->persistedContext), "%s",
                  task->checkpoint.requestContext);
    }
    http_context_id (task->extraJson, context->contextId, sizeof (context->contextId));
    if (http_apply_common (context, context->curl) != 0)
        goto failed;
    DOWNLOADER_CURL_SET (context, CURLOPT_WRITEFUNCTION, http_write);
    DOWNLOADER_CURL_SET (context, CURLOPT_WRITEDATA, context);
    DOWNLOADER_CURL_SET (context, CURLOPT_HEADERFUNCTION, http_header);
    DOWNLOADER_CURL_SET (context, CURLOPT_HEADERDATA, context);
    DOWNLOADER_CURL_SET (context, CURLOPT_XFERINFOFUNCTION, http_progress);
    DOWNLOADER_CURL_SET (context, CURLOPT_XFERINFODATA, context);
    DOWNLOADER_CURL_SET (context, CURLOPT_NOPROGRESS, 0L);
    int descriptor = open (task->tempPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (descriptor < 0)
        goto failed;
    struct stat status;
    int statResult = fstat (descriptor, &status);
    if (statResult != 0 || !S_ISREG (status.st_mode) || status.st_nlink != 1 || status.st_size < 0) {
        int savedErrno = statResult != 0 ? errno : EINVAL;
        (void)close (descriptor);
        errno = savedErrno;
        goto failed;
    }
    context->initialBytes = (uint64_t)status.st_size;
    context->durableBytes = context->initialBytes;
    context->file = fdopen (descriptor, "r+b");
    if (!context->file) {
        int savedErrno = errno;
        (void)close (descriptor);
        errno = savedErrno;
        goto failed;
    }
    *result = context;
    return 0;
failed:
    http_close (context);
    return -1;
}

static int http_prepare (ProtocolTask* task)
{
    struct stat status;
    if (http_flush (task) != 0)
        return -1;
    if (fstat (fileno (task->file), &status) != 0) {
        task->localError = errno;
        return -1;
    }
    if (status.st_nlink != 1) {
        task->localError = EINVAL;
        return -1;
    }
    task->initialBytes = (uint64_t)status.st_size;
    task->received = 0;
    task->status = 0;
    task->accepted = false;
    task->redirected = false;
    task->resumeRejected = false;
    task->rangeSeen = false;
    task->retryAfter = 0;
    if (task->isHttp && task->initialBytes && !http_validator (task) && http_reset_file (task) != 0)
        return -1;
    if (fseeko (task->file, 0, SEEK_END) != 0) {
        task->localError = errno;
        return -1;
    }
    DOWNLOADER_CURL_SET (task, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all (task->headers);
    task->headers = NULL;
    if (task->isHttp) {
        char range[64];
        snprintf (range, sizeof (range), "%" PRIu64 "-", task->initialBytes);
        DOWNLOADER_CURL_SET (task, CURLOPT_RANGE, task->initialBytes ? range : NULL);
        if (task->initialBytes) {
            char validator[544];
            snprintf (validator, sizeof (validator), "If-Range: %s", http_validator (task));
            task->headers = curl_slist_append (NULL, validator);
            if (!task->headers) {
                errno = ENOMEM;
                goto failed;
            }
            DOWNLOADER_CURL_SET (task, CURLOPT_HTTPHEADER, task->headers);
        }
    } else {
        DOWNLOADER_CURL_SET (task, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)task->initialBytes);
    }
    if (task->transferTimeout > 0) {
        int64_t now;
        if (c_time_monotonic_us (&now) != 0)
            goto failed;
        int64_t remaining = (int64_t)task->transferTimeout * 1000000 - (now - task->started);
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            goto failed;
        }
        long milliseconds = remaining / 1000 >= LONG_MAX ? LONG_MAX : (long)((remaining + 999) / 1000);
        DOWNLOADER_CURL_SET (task, CURLOPT_TIMEOUT_MS, milliseconds);
    }
    return 0;
failed:
    task->localError = errno ? errno : EIO;
    return -1;
}

static CURLcode http_perform (ProtocolTask* task)
{
    CURLM* multi = curl_multi_init ();
    if (!multi)
        return CURLE_OUT_OF_MEMORY;
    CURLMcode result = curl_multi_add_handle (multi, task->curl);
    bool added = result == CURLM_OK;
    CURLcode outcome = CURLE_FAILED_INIT;
    int running = 1;
    while (result == CURLM_OK && running) {
        if (http_progress (task, 0, 0, 0, 0)) {
            outcome = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        result = curl_multi_perform (multi, &running);
        if (result != CURLM_OK)
            break;
        int remaining;
        CURLMsg* message;
        while ((message = curl_multi_info_read (multi, &remaining)) != NULL) {
            if (message->msg == CURLMSG_DONE)
                outcome = message->data.result;
        }
        if (running)
            result = curl_multi_poll (multi, NULL, 0, 100, NULL);
    }
    if (result != CURLM_OK)
        outcome = CURLE_FAILED_INIT;
    if (added && curl_multi_remove_handle (multi, task->curl) != CURLM_OK)
        outcome = CURLE_FAILED_INIT;
    if (curl_multi_cleanup (multi) != CURLM_OK)
        outcome = CURLE_FAILED_INIT;
    return outcome;
}

static bool http_transient (CURLcode result, long status)
{
    if (status >= 400)
        return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
    return result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_RESOLVE_PROXY ||
           result == CURLE_COULDNT_CONNECT || result == CURLE_OPERATION_TIMEDOUT ||
           result == CURLE_PARTIAL_FILE || result == CURLE_RECV_ERROR;
}

static int http_wait (ProtocolTask* task, unsigned seconds)
{
    int64_t start;
    if (c_time_monotonic_us (&start) != 0) {
        task->localError = errno;
        return -1;
    }
    for (;;) {
        int64_t now;
        if (http_progress (task, 0, 0, 0, 0))
            return -1;
        if (c_time_monotonic_us (&now) != 0) {
            task->localError = errno;
            return -1;
        }
        if (now - start >= (int64_t)seconds * 1000000)
            return 0;
        struct timespec delay = { 0, 100000000 };
        if (nanosleep (&delay, NULL) != 0 && errno != EINTR) {
            task->localError = errno;
            return -1;
        }
    }
}

/* Three-connection HTTP(S) download. The segment files and their metadata are
 * the durable recovery state; the final Matroska/raw file is concatenated only
 * after every segment is complete, so the core's commit sees one stable file. */
#define DOWNLOADER_HTTP_PARALLEL_CONNECTIONS 3U
#define DOWNLOADER_HTTP_PARALLEL_MIN_SIZE    (1024U * 1024U)
#define DOWNLOADER_HTTP_SEGMENT_SYNC         (1024U * 1024U)

typedef struct HttpParallel HttpParallel;

typedef struct
{
    HttpParallel* parent;
    unsigned      index;
    uint64_t      start;
    uint64_t      end;
    uint64_t      have;
    uint64_t      durable;
    FILE*         file;
    CURL*         curl;
    bool          accepted;
    bool          rangeSeen;
    bool          lengthSeen;
    bool          badHeaders;
    bool          complete;
    long          status;
    uint64_t      rangeStart;
    uint64_t      rangeEnd;
    uint64_t      rangeTotal;
    uint64_t      contentLength;
    char          responseEtag[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char          responseModified[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    int           error;
    CURLcode      result;
    char          path[DOWNLOAD_TASK_PATH_SIZE];
} HttpSegment;

struct HttpParallel
{
    ProtocolTask* owner;
    CMutex        mutex;
    HttpSegment   segments[DOWNLOADER_HTTP_PARALLEL_CONNECTIONS];
    uint64_t      size;
    bool          stop;
    int           error;
    char          effectiveUrl[DOWNLOAD_TASK_URL_SIZE];
    char          etag[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char          modified[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char          directory[DOWNLOAD_TASK_PATH_SIZE];
    char          metaPath[DOWNLOAD_TASK_PATH_SIZE];
    char          contextId[DOWNLOAD_TASK_CONTEXT_SIZE];
    int64_t       startedAt;
};

static void http_segment_range (HttpParallel* parallel, unsigned index, uint64_t* start, uint64_t* end)
{
    *start = parallel->size * index / DOWNLOADER_HTTP_PARALLEL_CONNECTIONS;
    *end = parallel->size * (index + 1U) / DOWNLOADER_HTTP_PARALLEL_CONNECTIONS - 1U;
}

static bool http_parallel_validator (const char* etag, const char* modified)
{
    return http_strong_etag (etag) ||
           (modified[0] != '\0' && curl_getdate (modified, NULL) != (time_t)-1);
}

static void http_parallel_lock (HttpParallel* parallel, bool* stop)
{
    c_mutex_lock (&parallel->mutex);
    *stop = parallel->stop;
    c_mutex_unlock (&parallel->mutex);
}

static int http_parallel_save_meta (HttpParallel* parallel)
{
    FILE* file;
    char  temporary[DOWNLOAD_TASK_PATH_SIZE];
    uint64_t durable[DOWNLOADER_HTTP_PARALLEL_CONNECTIONS];
    char  effectiveUrl[DOWNLOAD_TASK_URL_SIZE];
    char  etag[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char  modified[DOWNLOAD_TASK_HTTP_VALUE_SIZE];

    c_mutex_lock (&parallel->mutex);
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++)
        durable[index] = parallel->segments[index].durable;
    snprintf (effectiveUrl, sizeof (effectiveUrl), "%s", parallel->effectiveUrl);
    snprintf (etag, sizeof (etag), "%s", parallel->etag);
    snprintf (modified, sizeof (modified), "%s", parallel->modified);
    c_mutex_unlock (&parallel->mutex);
    if (strpbrk (effectiveUrl, "\r\n") != NULL || strpbrk (etag, "\r\n") != NULL ||
        strpbrk (modified, "\r\n") != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf (temporary, sizeof (temporary), "%s.tmp", parallel->metaPath) >= (int)sizeof (temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    file = fopen (temporary, "w");
    if (file == NULL)
        return -1;
    int written = fprintf (file, "version=1\ncontext=%s\neffective_url=%s\nsize=%llu\netag=%s\n"
                                "last_modified=%s\n",
                          parallel->contextId, effectiveUrl, (unsigned long long)parallel->size, etag,
                          modified);
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        if (written >= 0)
            written = fprintf (file, "segment%u=%llu-%llu:%llu\n", index,
                               (unsigned long long)parallel->segments[index].start,
                               (unsigned long long)parallel->segments[index].end,
                               (unsigned long long)durable[index]);
    }
    if (written < 0 || fflush (file) != 0 || fsync (fileno (file)) != 0 || fclose (file) != 0) {
        int savedErrno = errno != 0 ? errno : EIO;
        (void)unlink (temporary);
        errno = savedErrno;
        return -1;
    }
    if (rename (temporary, parallel->metaPath) != 0)
        return -1;
    return 0;
}

static void http_parallel_discard (HttpParallel* parallel)
{
    if (parallel->directory[0] != '\0')
        (void)c_directory_remove_recursive (parallel->directory);
}

static int http_parallel_make_directory (ProtocolTask* task, HttpParallel* parallel)
{
    if (snprintf (parallel->directory, sizeof (parallel->directory), "%s.segments", task->tempPath) >=
        (int)sizeof (parallel->directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (c_directory_create_with_mode (parallel->directory, 0700) != 0 && errno != EEXIST)
        return -1;
    struct stat status;
    if (lstat (parallel->directory, &status) != 0 || !S_ISDIR (status.st_mode) ||
        S_ISLNK (status.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf (parallel->metaPath, sizeof (parallel->metaPath), "%s/meta", parallel->directory) >=
        (int)sizeof (parallel->metaPath)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int http_parallel_open_segments (HttpParallel* parallel, bool resume)
{
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        HttpSegment* segment = &parallel->segments[index];
        int descriptor;
        struct stat status;
        if (snprintf (segment->path, sizeof (segment->path), "%s/seg%u", parallel->directory, index) >=
            (int)sizeof (segment->path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        descriptor = open (segment->path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0)
            return -1;
        if (fstat (descriptor, &status) != 0 || !S_ISREG (status.st_mode) || status.st_nlink != 1) {
            int savedErrno = errno != 0 ? errno : EINVAL;
            (void)close (descriptor);
            errno = savedErrno;
            return -1;
        }
        if (resume) {
            if ((uint64_t)status.st_size < segment->durable ||
                ftruncate (descriptor, (off_t)segment->durable) != 0) {
                (void)close (descriptor);
                errno = EINVAL;
                return -1;
            }
            segment->have = segment->durable;
        } else {
            if (ftruncate (descriptor, 0) != 0) {
                int savedErrno = errno;
                (void)close (descriptor);
                errno = savedErrno;
                return -1;
            }
            segment->have = 0;
            segment->durable = 0;
        }
        segment->file = fdopen (descriptor, "r+b");
        if (segment->file == NULL) {
            int savedErrno = errno;
            (void)close (descriptor);
            errno = savedErrno;
            return -1;
        }
        if (fseeko (segment->file, (off_t)segment->have, SEEK_SET) != 0) {
            errno = EIO;
            return -1;
        }
        segment->complete = segment->have == segment->end - segment->start + 1;
    }
    return 0;
}

static bool http_parallel_load_meta (ProtocolTask* task, HttpParallel* parallel)
{
    FILE* file;
    (void)task;
    char  line[8192];
    uint64_t durable[DOWNLOADER_HTTP_PARALLEL_CONNECTIONS] = {0, 0, 0};
    unsigned seen = 0;
    bool versionOk = false;
    bool contextOk = false;

    file = fopen (parallel->metaPath, "r");
    if (file == NULL)
        return false;
    while (fgets (line, sizeof (line), file) != NULL) {
        line[strcspn (line, "\r\n")] = '\0';
        if (strncmp (line, "version=", 8) == 0) {
            versionOk = strcmp (line + 8, "1") == 0;
        } else if (strncmp (line, "context=", 8) == 0) {
            contextOk = strcmp (line + 8, parallel->contextId) == 0;
        } else if (strncmp (line, "effective_url=", 14) == 0) {
            if (strlen (line + 14) >= sizeof (parallel->effectiveUrl) ||
                strpbrk (line + 14, "\r\n") != NULL)
                goto invalid;
            snprintf (parallel->effectiveUrl, sizeof (parallel->effectiveUrl), "%s", line + 14);
        } else if (strncmp (line, "size=", 5) == 0) {
            char* end = NULL;
            errno = 0;
            parallel->size = strtoull (line + 5, &end, 10);
            if (errno != 0 || end == line + 5 || *end != '\0')
                goto invalid;
        } else if (strncmp (line, "etag=", 5) == 0) {
            snprintf (parallel->etag, sizeof (parallel->etag), "%s", line + 5);
        } else if (strncmp (line, "last_modified=", 14) == 0) {
            snprintf (parallel->modified, sizeof (parallel->modified), "%s", line + 14);
        } else if (strncmp (line, "segment", 7) == 0) {
            unsigned index;
            unsigned long long start, end, have;
            if (sscanf (line, "segment%u=%llu-%llu:%llu", &index, &start, &end, &have) != 4 ||
                index >= DOWNLOADER_HTTP_PARALLEL_CONNECTIONS)
                goto invalid;
            parallel->segments[index].start = (uint64_t)start;
            parallel->segments[index].end = (uint64_t)end;
            durable[index] = (uint64_t)have;
            seen++;
        }
    }
    fclose (file);
    file = NULL;
    if (!versionOk || !contextOk || parallel->size < DOWNLOADER_HTTP_PARALLEL_MIN_SIZE ||
        parallel->effectiveUrl[0] == '\0' || seen != DOWNLOADER_HTTP_PARALLEL_CONNECTIONS ||
        !http_parallel_validator (parallel->etag, parallel->modified))
        return false;
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        uint64_t start, end;
        http_segment_range (parallel, index, &start, &end);
        if (parallel->segments[index].start != start || parallel->segments[index].end != end ||
            durable[index] > end - start + 1)
            return false;
        parallel->segments[index].index = index;
        parallel->segments[index].parent = parallel;
        parallel->segments[index].durable = durable[index];
    }
    return true;
invalid:
    if (file != NULL)
        fclose (file);
    return false;
}

typedef struct
{
    long     status;
    uint64_t length;
    bool     lengthSeen;
    char     etag[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char     modified[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
} HttpProbe;

static size_t http_probe_header (char* header, size_t size, size_t count, void* userData)
{
    HttpProbe* probe = userData;
    size_t     length;
    char       line[8192];
    char*      colon;
    char*      end;

    if (size && count > SIZE_MAX / size)
        return 0;
    length = size * count;
    if (length >= 5 && !strncasecmp (header, "HTTP/", 5)) {
        char* space = memchr (header, ' ', length);
        if (!space || header + length - space < 4)
            return 0;
        probe->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        probe->lengthSeen = false;
        probe->etag[0] = 0;
        probe->modified[0] = 0;
        return length;
    }
    if ((length == 2 && !memcmp (header, "\r\n", 2)) || (length == 1 && header[0] == '\n'))
        return length;
    if (probe->status < 200 || probe->status >= 300)
        return length;
    if (length >= sizeof (line) || memchr (header, 0, length))
        return length;
    memcpy (line, header, length);
    line[length] = 0;
    colon = strchr (line, ':');
    if (!colon)
        return length;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    end = colon + strlen (colon);
    while (end > colon && strchr (" \t\r\n", end[-1]))
        *--end = 0;
    if (!strcasecmp (line, "Content-Length")) {
        const char* cursor = colon;
        uint64_t number;
        if (http_number (&cursor, &number) && *cursor == '\0') {
            probe->length = number;
            probe->lengthSeen = true;
        }
    } else if (!strcasecmp (line, "ETag")) {
        snprintf (probe->etag, sizeof (probe->etag), "%s", colon);
    } else if (!strcasecmp (line, "Last-Modified")) {
        snprintf (probe->modified, sizeof (probe->modified), "%s", colon);
    }
    return length;
}

static int http_parallel_probe (ProtocolTask* task, HttpParallel* parallel)
{
    CURL*        curl = curl_easy_init ();
    HttpProbe    probe;
    CURLcode     result;
    char*        effectiveUrl = NULL;

    if (curl == NULL)
        return -1;
    memset (&probe, 0, sizeof (probe));
    if (http_apply_common (task, curl) != 0)
        goto failed;
    if (curl_easy_setopt (curl, CURLOPT_NOBODY, 1L) != CURLE_OK ||
        curl_easy_setopt (curl, CURLOPT_HEADERFUNCTION, http_probe_header) != CURLE_OK ||
        curl_easy_setopt (curl, CURLOPT_HEADERDATA, &probe) != CURLE_OK ||
        curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 1L) != CURLE_OK)
        goto failed;
    result = curl_easy_perform (curl);
    if (result != CURLE_OK || probe.status < 200 || probe.status >= 300 || !probe.lengthSeen ||
        probe.length < DOWNLOADER_HTTP_PARALLEL_MIN_SIZE ||
        curl_easy_getinfo (curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl) != CURLE_OK ||
        effectiveUrl == NULL || strlen (effectiveUrl) >= sizeof (parallel->effectiveUrl) ||
        strpbrk (effectiveUrl, "\r\n") != NULL)
        goto failed;
    if (!http_safe_value (probe.etag, strlen (probe.etag)) ||
        !http_safe_value (probe.modified, strlen (probe.modified)))
        goto failed;
    snprintf (parallel->effectiveUrl, sizeof (parallel->effectiveUrl), "%s", effectiveUrl);
    snprintf (parallel->etag, sizeof (parallel->etag), "%s", probe.etag);
    snprintf (parallel->modified, sizeof (parallel->modified), "%s", probe.modified);
    parallel->size = probe.length;
    curl_easy_cleanup (curl);
    return 0;
failed:
    curl_easy_cleanup (curl);
    return -1;
}

static size_t http_segment_header (char* header, size_t size, size_t count, void* userData)
{
    HttpSegment* segment = userData;
    HttpParallel* parallel = segment->parent;
    size_t       length;
    char         line[8192];
    char*        colon;
    char*        end;

    if (size && count > SIZE_MAX / size)
        return 0;
    length = size * count;
    if (length >= 5 && !strncasecmp (header, "HTTP/", 5)) {
        char* space = memchr (header, ' ', length);
        if (!space || header + length - space < 4)
            return 0;
        segment->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        segment->accepted = false;
        segment->rangeSeen = false;
        segment->lengthSeen = false;
        segment->badHeaders = false;
        segment->responseEtag[0] = 0;
        segment->responseModified[0] = 0;
        return length;
    }
    if (segment->accepted)
        return length;
    if ((length == 2 && !memcmp (header, "\r\n", 2)) || (length == 1 && header[0] == '\n')) {
        uint64_t expectedStart;
        uint64_t expectedLength;
        if ((segment->status == 401 && parallel->owner->authNegotiating) ||
            (segment->status == 407 && parallel->owner->proxyNegotiating))
            return length;   /* Let libcurl answer the authentication challenge. */
        if (segment->status == 200 || segment->status == 412 || segment->status == 416) {
            /* Range was ignored or the validator no longer matches: the
             * single-stream path fetches the resource from scratch. */
            segment->error = ENOTSUP;
            return 0;
        }
        if (segment->status < 200 || segment->status >= 300) {
            segment->error = segment->status >= 400 ? EIO : EPROTO;
            return 0;
        }
        expectedStart = segment->start + segment->have;
        if (segment->end < expectedStart)
            segment->error = EPROTO;
        if (segment->error != 0)
            return 0;
        expectedLength = segment->end - expectedStart + 1;
        if (segment->status != 206 || segment->badHeaders || !segment->rangeSeen ||
            segment->rangeStart != expectedStart || segment->rangeEnd != segment->end ||
            segment->rangeTotal != parallel->size ||
            (segment->lengthSeen && segment->contentLength != expectedLength) ||
            (segment->have > 0 && http_strong_etag (parallel->etag) && segment->responseEtag[0] &&
             strcmp (parallel->etag, segment->responseEtag)) ||
            (segment->have > 0 && !http_strong_etag (parallel->etag) && parallel->modified[0] &&
             segment->responseModified[0] && strcmp (parallel->modified, segment->responseModified))) {
            segment->error = EPROTO;
            return 0;
        }
        segment->accepted = true;
        return length;
    }
    if (segment->status < 200 || (segment->status >= 300 && segment->status < 400))
        return length;
    if (length >= sizeof (line) || memchr (header, 0, length)) {
        segment->badHeaders = true;
        return length;
    }
    memcpy (line, header, length);
    line[length] = 0;
    colon = strchr (line, ':');
    if (!colon)
        return length;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    end = colon + strlen (colon);
    while (end > colon && strchr (" \t\r\n", end[-1]))
        *--end = 0;
    if (!strcasecmp (line, "Content-Range")) {
        const char* cursor = colon;
        if (segment->rangeSeen || strncmp (cursor, "bytes ", 6)) {
            segment->badHeaders = true;
        } else {
            cursor += 6;
            if (!http_number (&cursor, &segment->rangeStart) || *cursor++ != '-' ||
                !http_number (&cursor, &segment->rangeEnd) || *cursor++ != '/' ||
                !http_number (&cursor, &segment->rangeTotal) || *cursor)
                segment->badHeaders = true;
        }
        segment->rangeSeen = true;
    } else if (!strcasecmp (line, "Content-Length")) {
        const char* cursor = colon;
        uint64_t number;
        if (!http_number (&cursor, &number) || *cursor || (segment->lengthSeen && number != segment->contentLength))
            segment->badHeaders = true;
        else
            segment->contentLength = number;
        segment->lengthSeen = true;
    } else if (!strcasecmp (line, "ETag")) {
        if (!http_safe_value (colon, strlen (colon)) || (segment->responseEtag[0] && strcmp (segment->responseEtag, colon)))
            segment->badHeaders = true;
        else
            snprintf (segment->responseEtag, sizeof (segment->responseEtag), "%s", colon);
    } else if (!strcasecmp (line, "Last-Modified")) {
        if (!http_safe_value (colon, strlen (colon)) ||
            (segment->responseModified[0] && strcmp (segment->responseModified, colon)))
            segment->badHeaders = true;
        else
            snprintf (segment->responseModified, sizeof (segment->responseModified), "%s", colon);
    }
    return length;
}

static size_t http_segment_write (const void* bytes, size_t size, size_t count, void* userData)
{
    HttpSegment*  segment = userData;
    HttpParallel* parallel = segment->parent;
    size_t        length;
    size_t        written;
    bool          stop;

    if (size && count > SIZE_MAX / size)
        return 0;
    length = size * count;
    http_parallel_lock (parallel, &stop);
    if (stop)
        return 0;
    if (!segment->accepted)
        return length;
    uint64_t expectedLength = segment->end - segment->start + 1;
    if (segment->have > expectedLength || length > expectedLength - segment->have) {
        segment->error = EPROTO;
        return 0;
    }
    written = fwrite (bytes, 1, length, segment->file);
    segment->have += written;
    if (written != length) {
        segment->error = errno != 0 ? errno : EIO;
        return written;
    }
    if (segment->have - segment->durable >= DOWNLOADER_HTTP_SEGMENT_SYNC) {
        if (fflush (segment->file) != 0 || fsync (fileno (segment->file)) != 0) {
            segment->error = errno != 0 ? errno : EIO;
            return 0;
        }
        c_mutex_lock (&parallel->mutex);
        segment->durable = segment->have;
        c_mutex_unlock (&parallel->mutex);
    }
    return written;
}

static int http_segment_progress (void* userData, curl_off_t total, curl_off_t downloaded,
                                  curl_off_t uploadTotal, curl_off_t uploaded)
{
    HttpSegment* segment = userData;
    bool         stop;
    (void)total;
    (void)downloaded;
    (void)uploadTotal;
    (void)uploaded;
    http_parallel_lock (segment->parent, &stop);
    return stop ? 1 : 0;
}

static const ProtocolCallbacks gcEmptyCallbacks = {0};

static void http_segment_thread (void* userData)
{
    HttpSegment*  segment = userData;
    HttpParallel* parallel = segment->parent;
    struct curl_slist* headers = NULL;

    segment->curl = curl_easy_init ();
    if (segment->curl == NULL) {
        segment->error = ENOMEM;
        goto finished;
    }
    if (http_apply_common (parallel->owner, segment->curl) != 0) {
        segment->error = errno != 0 ? errno : EIO;
        goto finished;
    }
    {
        char range[64];
        uint64_t first = segment->start + segment->have;
        if (segment->complete)
            goto finished;
        snprintf (range, sizeof (range), "%llu-%llu", (unsigned long long)first,
                  (unsigned long long)segment->end);
        if (curl_easy_setopt (segment->curl, CURLOPT_RANGE, range) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_WRITEFUNCTION, http_segment_write) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_WRITEDATA, segment) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_HEADERFUNCTION, http_segment_header) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_HEADERDATA, segment) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_XFERINFOFUNCTION, http_segment_progress) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_XFERINFODATA, segment) != CURLE_OK ||
            curl_easy_setopt (segment->curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK) {
            segment->error = EIO;
            goto finished;
        }
        if (segment->have > 0) {
            char validator[544];
            snprintf (validator, sizeof (validator), "If-Range: %s", http_strong_etag (parallel->etag)
                          ? parallel->etag : parallel->modified);
            headers = curl_slist_append (NULL, validator);
            if (headers == NULL || curl_easy_setopt (segment->curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK) {
                segment->error = ENOMEM;
                goto finished;
            }
        }
    }
    segment->result = curl_easy_perform (segment->curl);
    if (segment->result == CURLE_OK && segment->accepted && segment->error == 0) {
        char*    effectiveUrl = NULL;
        uint64_t expectedLength = segment->end - segment->start + 1;
        if (curl_easy_getinfo (segment->curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl) != CURLE_OK ||
            effectiveUrl == NULL || strcmp (effectiveUrl, parallel->effectiveUrl) != 0) {
            segment->error = EPROTO;
        } else if (segment->have != expectedLength) {
            segment->error = EPROTO;
        } else if (fflush (segment->file) != 0 || fsync (fileno (segment->file)) != 0) {
            segment->error = errno != 0 ? errno : EIO;
        } else {
            c_mutex_lock (&parallel->mutex);
            segment->durable = segment->have;
            segment->complete = true;
            c_mutex_unlock (&parallel->mutex);
            segment->error = 0;
        }
    } else if (segment->error == 0) {
        segment->error = segment->result == CURLE_OPERATION_TIMEDOUT ? ETIMEDOUT : EIO;
    }
finished:
    curl_slist_free_all (headers);
    if (segment->curl != NULL) {
        curl_easy_cleanup (segment->curl);
        segment->curl = NULL;
    }
    if (segment->error != 0) {
        c_mutex_lock (&parallel->mutex);
        if (!parallel->stop) {
            parallel->stop = true;
            parallel->error = segment->error;
        }
        c_mutex_unlock (&parallel->mutex);
    }
}

static int http_parallel_concat (HttpParallel* parallel, ProtocolTask* task)
{
    if (task->file == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (fflush (task->file) != 0 || ftruncate (fileno (task->file), 0) != 0 ||
        fseeko (task->file, 0, SEEK_SET) != 0) {
        task->localError = errno != 0 ? errno : EIO;
        return -1;
    }
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        HttpSegment* segment = &parallel->segments[index];
        uint64_t     remaining = segment->have;
        if (segment->file == NULL)
            continue;
        if (fflush (segment->file) != 0 || fseeko (segment->file, 0, SEEK_SET) != 0) {
            task->localError = errno != 0 ? errno : EIO;
            return -1;
        }
        while (remaining > 0) {
            unsigned char buffer[65536];
            size_t        chunk = remaining > sizeof (buffer) ? sizeof (buffer) : (size_t)remaining;
            if (fread (buffer, 1, chunk, segment->file) != chunk ||
                fwrite (buffer, 1, chunk, task->file) != chunk) {
                task->localError = errno != 0 ? errno : EIO;
                return -1;
            }
            remaining -= chunk;
        }
    }
    if (fflush (task->file) != 0 || fsync (fileno (task->file)) != 0) {
        task->localError = errno != 0 ? errno : EIO;
        return -1;
    }
    return 0;
}

/* Returns 0 with *fallback=true when the resource cannot use three connections;
 * the caller then runs the single-stream path. */
static int http_run_parallel (ProtocolTask* task, bool* fallback)
{
    HttpParallel parallel;
    CThread*     threads[DOWNLOADER_HTTP_PARALLEL_CONNECTIONS] = {NULL, NULL, NULL};
    bool         resume;
    bool         stopped = false;
    int          result = -1;
    int          error = 0;
    int          savedErrno = 0;

    *fallback = false;
    const ProtocolCallbacks* callbacks = task->callbacks != NULL ? task->callbacks : &gcEmptyCallbacks;
    memset (&parallel, 0, sizeof (parallel));
    parallel.owner = task;
    if (!c_mutex_init (&parallel.mutex)) {
        errno = ENOMEM;
        return -1;
    }
    http_context_id (task->extraJson, parallel.contextId, sizeof (parallel.contextId));
    if (http_parallel_make_directory (task, &parallel) != 0) {
        c_mutex_clear (&parallel.mutex);
        *fallback = true;
        return 0;
    }
    resume = http_parallel_load_meta (task, &parallel);
    if (resume && http_parallel_open_segments (&parallel, true) != 0) {
        http_parallel_discard (&parallel);
        resume = false;
    }
    if (!resume) {
        http_parallel_discard (&parallel);
        if (http_parallel_make_directory (task, &parallel) != 0 || http_parallel_probe (task, &parallel) != 0) {
            http_parallel_discard (&parallel);
            c_mutex_clear (&parallel.mutex);
            *fallback = true;
            return 0;
        }
        for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
            uint64_t start, end;
            http_segment_range (&parallel, index, &start, &end);
            parallel.segments[index].parent = &parallel;
            parallel.segments[index].index = index;
            parallel.segments[index].start = start;
            parallel.segments[index].end = end;
            parallel.segments[index].durable = 0;
        }
        if (http_parallel_open_segments (&parallel, false) != 0) {
            http_parallel_discard (&parallel);
            c_mutex_clear (&parallel.mutex);
            *fallback = true;
            return 0;
        }
    }
    if (http_parallel_save_meta (&parallel) != 0) {
        http_parallel_discard (&parallel);
        c_mutex_clear (&parallel.mutex);
        *fallback = true;
        return 0;
    }
    if (callbacks->onStarted != NULL)
        callbacks->onStarted (task->userData);
    if (callbacks->onMetadata != NULL && (parallel.etag[0] || parallel.modified[0])) {
        errno = 0;
        if (callbacks->onMetadata (task->userData, parallel.etag, parallel.modified) != 0) {
            http_parallel_discard (&parallel);
            c_mutex_clear (&parallel.mutex);
            return -1;
        }
    }
    int64_t  progressAt = 0;
    uint64_t progressBytes = 0;
    if (c_time_monotonic_us (&progressAt) != 0) {
        errno = EIO;
        http_parallel_discard (&parallel);
        c_mutex_clear (&parallel.mutex);
        return -1;
    }
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        if (parallel.segments[index].complete)
            continue;
        threads[index] = c_thread_new_named ("downloader-http-seg", http_segment_thread, &parallel.segments[index]);
        if (threads[index] == NULL) {
            c_mutex_lock (&parallel.mutex);
            parallel.stop = true;
            c_mutex_unlock (&parallel.mutex);
            for (unsigned join = 0; join < index; join++) {
                if (threads[join] != NULL) {
                    (void)c_thread_join (threads[join]);
                    (void)c_thread_free (threads[join]);
                }
            }
            http_parallel_discard (&parallel);
            c_mutex_clear (&parallel.mutex);
            *fallback = true;
            return 0;
        }
    }
    for (;;) {
        uint64_t downloaded = 0;
        bool     done = true;
        c_mutex_lock (&parallel.mutex);
        for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
            downloaded += parallel.segments[index].have;
            if (!parallel.segments[index].complete)
                done = false;
        }
        error = parallel.error;
        c_mutex_unlock (&parallel.mutex);
        if (task->pauseRequested || task->cancelRequested) {
            c_mutex_lock (&parallel.mutex);
            parallel.stop = true;
            c_mutex_unlock (&parallel.mutex);
            stopped = true;
            break;
        }
        if (task->transferTimeout > 0) {
            int64_t now;
            if (c_time_monotonic_us (&now) == 0 &&
                now - parallel.startedAt >= (int64_t)task->transferTimeout * 1000000) {
                c_mutex_lock (&parallel.mutex);
                if (!parallel.stop) {
                    parallel.stop = true;
                    parallel.error = ETIMEDOUT;
                }
                c_mutex_unlock (&parallel.mutex);
                error = ETIMEDOUT;
            }
        }
        {
            int64_t now;
            if (c_time_monotonic_us (&now) == 0 && now - progressAt >= 100000) {
                uint64_t speed = 0;
                if (now > progressAt && downloaded >= progressBytes)
                    speed = (downloaded - progressBytes) * 1000000U / (uint64_t)(now - progressAt);
                progressAt = now;
                progressBytes = downloaded;
                if (callbacks->onProgress != NULL)
                    callbacks->onProgress (task->userData, parallel.size, downloaded, speed);
            }
        }
        if (error != 0 || done)
            break;
        struct timespec delay = { 0, 50000000 };
        (void)nanosleep (&delay, NULL);
    }
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        if (threads[index] != NULL) {
            (void)c_thread_join (threads[index]);
            (void)c_thread_free (threads[index]);
        }
    }
    c_mutex_lock (&parallel.mutex);
    if (parallel.error != 0 && error == 0)
        error = parallel.error;
    c_mutex_unlock (&parallel.mutex);
    (void)http_parallel_save_meta (&parallel);
    (void)http_parallel_concat (&parallel, task);
    if (stopped) {
        if (task->pauseRequested) {
            if (callbacks->onPaused != NULL)
                callbacks->onPaused (task->userData);
            result = 1;
        } else {
            errno = ECANCELED;
            result = -1;
        }
    } else if (error == ENOTSUP) {
        /* The server ignored Range; the single-stream path downloads it. */
        http_parallel_discard (&parallel);
        if (task->file != NULL) {
            (void)fflush (task->file);
            (void)ftruncate (fileno (task->file), 0);
            (void)fseeko (task->file, 0, SEEK_SET);
        }
        *fallback = true;
        result = 0;
    } else if (error != 0) {
        for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
            if (parallel.segments[index].error != 0) {
                task->status = parallel.segments[index].status;
                task->lastResult = parallel.segments[index].result;
                error = parallel.segments[index].error;
                break;
            }
        }
        savedErrno = error;
        if (callbacks->onFailed != NULL) {
            char message[DOWNLOAD_TASK_ERROR_SIZE];
            snprintf (message, sizeof (message), "HTTP %ld; curl %d (%s); errno %d (%s)", task->status,
                      (int)task->lastResult, curl_easy_strerror (task->lastResult), error, strerror (error));
            callbacks->onFailed (task->userData, error, message);
        }
        errno = savedErrno;
        result = -1;
    } else {
        http_parallel_discard (&parallel);
        if (callbacks->onCompleted != NULL)
            callbacks->onCompleted (task->userData);
        result = 0;
    }
    for (unsigned index = 0; index < DOWNLOADER_HTTP_PARALLEL_CONNECTIONS; index++) {
        if (parallel.segments[index].file != NULL) {
            (void)fclose (parallel.segments[index].file);
            parallel.segments[index].file = NULL;
        }
    }
    c_mutex_clear (&parallel.mutex);
    return result;
}
static int http_run_sequential (ProtocolTask* task);

static bool http_parallel_sidecar_exists (ProtocolTask* task)
{
    char        path[DOWNLOAD_TASK_PATH_SIZE];
    struct stat status;

    if (snprintf (path, sizeof (path), "%s.segments/meta", task->tempPath) >= (int)sizeof (path))
        return false;
    return stat (path, &status) == 0 && S_ISREG (status.st_mode);
}

static int http_run (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (task->isHttp && (task->initialBytes == 0 || http_parallel_sidecar_exists (task))) {
        bool fallback = false;
        int  result = http_run_parallel (task, &fallback);
        if (!fallback)
            return result;
        if (task->file != NULL) {
            (void)fflush (task->file);
            (void)ftruncate (fileno (task->file), 0);
            (void)fseeko (task->file, 0, SEEK_SET);
        }
    }
    return http_run_sequential (task);
}

static int http_run_sequential (ProtocolTask* task)
{
    if (!task) {
        errno = EINVAL;
        return -1;
    }
    CURLcode result = CURLE_OK;
    if (c_time_monotonic_us (&task->started) != 0)
        task->localError = errno;
    if (task->callbacks && task->callbacks->onStarted)
        task->callbacks->onStarted (task->userData);
    bool restarted416 = false;
    bool restartedResume = false;
    for (int attempt = 0; !task->localError; ) {
        if (http_progress (task, 0, 0, 0, 0) || http_prepare (task) != 0)
            break;
        result = http_perform (task);
        if (http_flush (task) != 0 || task->localError || task->pauseRequested || task->cancelRequested)
            break;
        if (task->isHttp) {
            if (task->status >= 400 || (result == CURLE_OK && !task->accepted))
                result = CURLE_HTTP_RETURNED_ERROR;
            else if (result == CURLE_OK && task->status == 206 &&
                     task->received != task->rangeEnd - task->rangeStart + 1)
                result = CURLE_PARTIAL_FILE;
        }
        if (result == CURLE_OK)
            break;
        bool restart416 = task->isHttp && task->status == 416 && task->initialBytes && !restarted416;
        if (task->resumeRejected && restartedResume) {
            task->localError = EPROTO;
            break;
        }
        if (restart416 || task->resumeRejected) {
            /* Representation recovery is distinct from transient retries:
             * once per cause per run, no backoff/count/budget consumption.
             * The overall timeout and pause/cancel still apply. */
            if (restart416)
                restarted416 = true;
            else
                restartedResume = true;
            if (http_reset_file (task) != 0)
                break;
            task->etag[0] = 0;
            task->modified[0] = 0;
            /* Persist the durable reset so a crash cannot resurrect bytes that
             * the new representation will replace. */
            if (http_checkpoint (task) != 0)
                break;
            continue;
        }
        if (!http_transient (result, task->status) || attempt >= task->maxRetries)
            break;
        attempt++;
        if (task->callbacks && task->callbacks->onRetry) {
            errno = 0;
            if (task->callbacks->onRetry (task->userData, attempt) != 0) {
                task->localError = errno ? errno : ECANCELED;
                break;
            }
        }
        if (task->pauseRequested || task->cancelRequested)
            break;
        unsigned delay = attempt > 6 ? 60 : 1U << (attempt - 1);
        if (task->retryAfter > delay)
            delay = task->retryAfter;
        if (http_wait (task, delay) != 0)
            break;
    }
    /* A pause or a transfer error still contributes its durable bytes to the
     * next resume. A checkpoint failure is recorded in localError. */
    if (task->file != NULL)
        (void)http_checkpoint (task);
    if (fclose (task->file) != 0 && !task->localError)
        task->localError = errno ? errno : EIO;
    task->file = NULL;
    if (task->pauseRequested) {
        if (task->callbacks && task->callbacks->onPaused)
            task->callbacks->onPaused (task->userData);
        return 1;
    }
    if (task->cancelRequested && !task->localError)
        task->localError = ECANCELED;
    if (!task->localError && result == CURLE_OK) {
        if (task->callbacks && task->callbacks->onCompleted)
            task->callbacks->onCompleted (task->userData);
        return 0;
    }
    task->lastResult = result;
    int savedErrno = task->localError ? task->localError : result == CURLE_OPERATION_TIMEDOUT ? ETIMEDOUT : EIO;
    if (task->callbacks && task->callbacks->onFailed) {
        char message[256];
        snprintf (message, sizeof (message), "HTTP %ld; curl %d (%s); errno %d (%s)", task->status,
                  (int)result, curl_easy_strerror (result), savedErrno, strerror (savedErrno));
        task->callbacks->onFailed (task->userData, task->localError ? task->localError : (int)result, message);
    }
    errno = savedErrno;
    return -1;
}

static int http_pause (ProtocolTask* task)
{
    if (!task) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = true;
    return 0;
}

static int http_resume (ProtocolTask* task)
{
    if (!task) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = false;
    return 0;
}

static int http_cancel (ProtocolTask* task)
{
    if (!task) {
        errno = EINVAL;
        return -1;
    }
    task->cancelRequested = true;
    return 0;
}

static bool http_retryable (ProtocolTask* task, int errorCode, const char* message)
{
    (void)message;
    if (task == NULL)
        return false;
    if (task->isHttp && (task->status == 408 || task->status == 429 ||
                         (task->status >= 500 && task->status < 600)))
        return true;
    switch (task->lastResult) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
        return true;
    default:
        break;
    }
    return errorCode == ETIMEDOUT || errorCode == ECONNRESET || errorCode == ECONNREFUSED ||
           errorCode == EHOSTUNREACH || errorCode == ENETUNREACH || errorCode == ENETDOWN ||
           errorCode == EPIPE || errorCode == EAGAIN || errorCode == ECONNABORTED;
}

static const ProtocolOps gcHttpProtocolOps = {
    .name = "curl",
    .canHandle = http_can_handle,
    .open = http_open,
    .run = http_run,
    .pause = http_pause,
    .resume = http_resume,
    .cancel = http_cancel,
    .close = http_close,
    .retryable = http_retryable
};

const ProtocolOps* http_protocol_ops (void)
{
    return &gcHttpProtocolOps;
}
