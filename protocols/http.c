#define _POSIX_C_SOURCE 200809L
#include "protocol.h"

#include <clib/memory.h>

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <clib/uri.h>

static size_t http_header (char* header, size_t size, size_t count, void* userData);

struct ProtocolTask
{
    CURL*                 curl;
    FILE*                 file;
    const ProtocolCallbacks* callbacks;
    void*                 userData;
    volatile bool         pauseRequested;
    volatile bool         cancelRequested;
    uint64_t              initialBytes;
};

static bool http_can_handle (const char* url, const char* protocol)
{
    (void)url;
    return protocol != NULL && (strcmp (protocol, "http") == 0 || strcmp (protocol, "https") == 0 ||
                                strcmp (protocol, "ftp") == 0 || strcmp (protocol, "ftps") == 0 ||
                                strcmp (protocol, "sftp") == 0);
}

static size_t http_write (const void* data, size_t size, size_t count, void* userData)
{
    ProtocolTask* task = (ProtocolTask*)userData;

    return fwrite (data, 1, size * count, task->file);
}

static int http_progress (void* userData, curl_off_t total, curl_off_t downloaded, curl_off_t uploadTotal, curl_off_t uploaded)
{
    ProtocolTask* task = (ProtocolTask*)userData;
    curl_off_t speed = 0;
    (void)uploadTotal;
    (void)uploaded;
    curl_easy_getinfo (task->curl, CURLINFO_SPEED_DOWNLOAD_T, &speed);

    if (task->cancelRequested || task->pauseRequested)
        return 1;
    if (task->callbacks != NULL && task->callbacks->onProgress != NULL) {
        task->callbacks->onProgress (task->userData, total > 0 ? task->initialBytes + (uint64_t)total : 0, task->initialBytes + (uint64_t)downloaded,
                                     (uint64_t)(speed < 0 ? 0 : speed));
    }
    return 0;
}

static FILE* http_open_partial (const DownloadTask* task)
{
    struct stat status;
    int descriptor = open (task->tempPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                            (task->downloadedBytes > 0 ? O_APPEND : 0), 0600);
    if (descriptor < 0)
        return NULL;
    int statResult = fstat (descriptor, &status);
    if (statResult != 0 || !S_ISREG (status.st_mode) || status.st_nlink != 1) {
        int savedErrno = statResult != 0 ? errno : EINVAL;
        (void)close (descriptor);
        errno = savedErrno;
        return NULL;
    }
    if (task->downloadedBytes == 0 && ftruncate (descriptor, 0) != 0) {
        int savedErrno = errno;
        (void)close (descriptor);
        errno = savedErrno;
        return NULL;
    }
    FILE* file = fdopen (descriptor, task->downloadedBytes == 0 ? "wb" : "ab");
    if (file == NULL) {
        int savedErrno = errno;
        (void)close (descriptor);
        errno = savedErrno;
    }
    return file;
}

static int http_open (ProtocolTask** result, const DownloadTask* task, const ProtocolCallbacks* callbacks,
                      void* userData)
{
    ProtocolTask* context;
    CURLcode      curlResult;

    if (result == NULL || task == NULL || !http_can_handle (task->resourceUrl, task->protocol) ||
        task->tempPath[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    curlResult = curl_global_init (CURL_GLOBAL_DEFAULT);
    if (curlResult != CURLE_OK) {
        errno = EIO;
        return -1;
    }
    context = (ProtocolTask*)c_malloc0 (sizeof (*context));
    if (context == NULL)
        return -1;
    context->curl = curl_easy_init ();
    context->file = http_open_partial (task);
    context->callbacks = callbacks;
    context->userData = userData;
    context->initialBytes = task->downloadedBytes;
    if (context->curl == NULL || context->file == NULL) {
        int savedErrno = errno == 0 ? EIO : errno;
        if (context->curl != NULL)
            curl_easy_cleanup (context->curl);
        if (context->file != NULL)
            (void)fclose (context->file);
        c_free (context);
        errno = savedErrno;
        return -1;
    }
    (void)curl_easy_setopt (context->curl, CURLOPT_URL, task->resourceUrl);
    (void)curl_easy_setopt (context->curl, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt (context->curl, CURLOPT_FAILONERROR, 1L);
    (void)curl_easy_setopt (context->curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt (context->curl, CURLOPT_CONNECTTIMEOUT, 30L);
    (void)curl_easy_setopt (context->curl, CURLOPT_WRITEFUNCTION, http_write);
    (void)curl_easy_setopt (context->curl, CURLOPT_HEADERFUNCTION, http_header);
    (void)curl_easy_setopt (context->curl, CURLOPT_HEADERDATA, context);
    (void)curl_easy_setopt (context->curl, CURLOPT_WRITEDATA, context);
    (void)curl_easy_setopt (context->curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    (void)curl_easy_setopt (context->curl, CURLOPT_XFERINFODATA, context);
    (void)curl_easy_setopt (context->curl, CURLOPT_NOPROGRESS, 0L);
    if (task->downloadedBytes > 0)
        (void)curl_easy_setopt (context->curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)task->downloadedBytes);
    *result = context;
    return 0;
}

static size_t http_header (char* header, size_t size, size_t count, void* userData)
{
    ProtocolTask* task = userData;
    size_t length = size * count;
    char line[4096];
    char* name = NULL;
    bool encoded = false;
    if (length >= sizeof (line) || length < 20 || strncasecmp (header, "Content-Disposition:", 20) != 0)
        return length;
    memcpy (line, header, length);
    line[length] = '\0';
    for (char* part = line + 20; *part; part++) {
        if (strncasecmp (part, "filename*=UTF-8''", 17) == 0) {
            name = part + 17;
            encoded = true;
            break;
        }
        if (strncasecmp (part, "filename=", 9) == 0)
            name = part + 9;
    }
    if (name != NULL) {
        if (*name == '"') {
            name++;
            char* end = strchr (name, '"');
            if (end != NULL)
                *end = '\0';
        } else {
            name[strcspn (name, ";\r\n")] = '\0';
        }
        char* decoded = NULL;
        size_t decodedSize = 0;
        if (encoded && c_uri_unescape_string (c_string_view_from_cstr (name), &decoded, &decodedSize) == 0) {
            if (memchr (decoded, '\0', decodedSize) == NULL)
                name = decoded;
        }
        if (task->callbacks && task->callbacks->onFileName)
            task->callbacks->onFileName (task->userData, name);
        c_free (decoded);
    }
    return length;
}

static int http_run (ProtocolTask* task)
{
    CURLcode result;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (task->callbacks != NULL && task->callbacks->onStarted != NULL)
        task->callbacks->onStarted (task->userData);
    result = curl_easy_perform (task->curl);
    if (fclose (task->file) != 0 && result == CURLE_OK)
        result = CURLE_WRITE_ERROR;
    task->file = NULL;
    if (result == CURLE_OK) {
        if (task->callbacks != NULL && task->callbacks->onCompleted != NULL)
            task->callbacks->onCompleted (task->userData);
        return 0;
    }
    if (task->pauseRequested) {
        if (task->callbacks != NULL && task->callbacks->onPaused != NULL)
            task->callbacks->onPaused (task->userData);
        return 1;
    }
    if (task->callbacks != NULL && task->callbacks->onFailed != NULL)
        task->callbacks->onFailed (task->userData, (int)result, curl_easy_strerror (result));
    errno = EIO;
    return -1;
}

static int http_pause (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = true;
    return 0;
}

static int http_resume (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = false;
    return 0;
}

static int http_cancel (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->cancelRequested = true;
    return 0;
}

static void http_close (ProtocolTask* task)
{
    if (task == NULL)
        return;
    if (task->file != NULL)
        (void)fclose (task->file);
    if (task->curl != NULL)
        curl_easy_cleanup (task->curl);
    c_free (task);
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
