#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <clib/json.h>
#include <clib/memory.h>
#include <clib/once.h>
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

/* All task callbacks and pause/cancel operations run on the owning worker. */
struct ProtocolTask
{
    CURL*                    curl;
    FILE*                    file;
    struct curl_slist*       headers;
    const ProtocolCallbacks* callbacks;
    void*                    userData;
    bool                     pauseRequested;
    bool                     cancelRequested;
    bool                     isHttp;
    bool                     accepted;
    bool                     rangeSeen;
    bool                     lengthSeen;
    bool                     badHeaders;
    bool                     encoded;
    bool                     redirected;
    bool                     redirectResumeRejected;
    int                      localError;
    int                      maxRetries;
    int                      transferTimeout;
    int64_t                  started;
    long                     status;
    uint64_t                 initialBytes;
    uint64_t                 received;
    uint64_t                 rangeStart;
    uint64_t                 rangeEnd;
    uint64_t                 rangeTotal;
    uint64_t                 contentLength;
    uint64_t                 knownTotal;
    unsigned                 retryAfter;
    char                     etag[513];
    char                     modified[513];
    char                     responseEtag[513];
    char                     responseModified[513];
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
    task->knownTotal = 0;
    return 0;
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
     * streaming error page cannot indefinitely postpone retry or recovery. */
    if (task->status >= 400)
        return -1;
    if (task->status < 200 || task->status >= 300)
        return 0;
    if (task->badHeaders || task->encoded) {
        task->localError = task->encoded ? ENOTSUP : EPROTO;
        return -1;
    }
    if (task->status == 206) {
        /* Validators are not bound to an effective URL in the database.
         * Even equal ETags at different redirect targets prove nothing. */
        if (task->initialBytes && task->redirected) {
            task->redirectResumeRejected = true;
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
    /* A later run may resolve the original URL to another resource, or no
     * longer redirect at all. Never persist target-scoped validators. */
    if (task->redirected) {
        task->etag[0] = 0;
        task->modified[0] = 0;
    }
    if (task->callbacks && task->callbacks->onMetadata) {
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

static int http_options (ProtocolTask* context, const char* extra)
{
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
        if (curl_easy_setopt (context->curl, curlOptions[index], option) != CURLE_OK) {
            errno = EIO;
            goto done;
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
    DOWNLOADER_CURL_SET (context, CURLOPT_URL, task->resourceUrl);
    DOWNLOADER_CURL_SET (context, CURLOPT_USERAGENT, "downloader/0.1.0");
    if (http_options (context, task->extraJson) != 0)
        goto failed;
    DOWNLOADER_CURL_SET (context, CURLOPT_FOLLOWLOCATION, 1L);
    DOWNLOADER_CURL_SET (context, CURLOPT_MAXREDIRS, 10L);
#if LIBCURL_VERSION_NUM >= 0x075500
    DOWNLOADER_CURL_SET (context, CURLOPT_PROTOCOLS_STR, context->isHttp ? "http,https" : task->protocol);
    DOWNLOADER_CURL_SET (context, CURLOPT_REDIR_PROTOCOLS_STR, context->isHttp ? "http,https" : task->protocol);
#else
    long protocols = context->isHttp ? CURLPROTO_HTTP | CURLPROTO_HTTPS :
        (!strcmp (task->protocol, "ftp") ? CURLPROTO_FTP : !strcmp (task->protocol, "ftps") ? CURLPROTO_FTPS : CURLPROTO_SFTP);
    DOWNLOADER_CURL_SET (context, CURLOPT_PROTOCOLS, protocols);
    DOWNLOADER_CURL_SET (context, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
    DOWNLOADER_CURL_SET (context, CURLOPT_SSL_VERIFYPEER, 1L);
    DOWNLOADER_CURL_SET (context, CURLOPT_SSL_VERIFYHOST, 2L);
    DOWNLOADER_CURL_SET (context, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    DOWNLOADER_CURL_SET (context, CURLOPT_NOSIGNAL, 1L);
    DOWNLOADER_CURL_SET (context, CURLOPT_CONNECTTIMEOUT, (long)(task->connectTimeout > 0 ? task->connectTimeout : 30));
    DOWNLOADER_CURL_SET (context, CURLOPT_LOW_SPEED_LIMIT, 1L);
    DOWNLOADER_CURL_SET (context, CURLOPT_LOW_SPEED_TIME, 60L);
    DOWNLOADER_CURL_SET (context, CURLOPT_ACCEPT_ENCODING, "identity");
    DOWNLOADER_CURL_SET (context, CURLOPT_HTTP_CONTENT_DECODING, 0L);
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
    task->redirectResumeRejected = false;
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

static int http_run (ProtocolTask* task)
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
    bool restartedRedirect = false;
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
        if (task->redirectResumeRejected && restartedRedirect) {
            task->localError = EPROTO;
            break;
        }
        if (restart416 || task->redirectResumeRejected) {
            /* Representation recovery is distinct from transient retries:
             * once per cause per run, no backoff/count/budget consumption.
             * The overall timeout and pause/cancel still apply. */
            if (restart416)
                restarted416 = true;
            else
                restartedRedirect = true;
            if (http_reset_file (task) != 0)
                break;
            task->etag[0] = 0;
            task->modified[0] = 0;
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

static const ProtocolOps gcHttpProtocolOps = {
    .name = "curl",
    .canHandle = http_can_handle,
    .open = http_open,
    .run = http_run,
    .pause = http_pause,
    .resume = http_resume,
    .cancel = http_cancel,
    .close = http_close
};

const ProtocolOps* http_protocol_ops (void)
{
    return &gcHttpProtocolOps;
}
