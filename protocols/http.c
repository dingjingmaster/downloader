#include "protocol.h"

#include <clib/memory.h>

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

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

    return fwrite (data, size, count, task->file);
}

static int http_progress (void* userData, curl_off_t total, curl_off_t downloaded, curl_off_t speed)
{
    ProtocolTask* task = (ProtocolTask*)userData;

    if (task->cancelRequested || task->pauseRequested)
        return 1;
    if (task->callbacks != NULL && task->callbacks->onProgress != NULL) {
        task->callbacks->onProgress (task->userData, (uint64_t)total, task->initialBytes + (uint64_t)downloaded,
                                     (uint64_t)(speed < 0 ? 0 : speed));
    }
    return 0;
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
    context->file = fopen (task->tempPath, task->downloadedBytes == 0 ? "wb" : "ab");
    context->callbacks = callbacks;
    context->userData = userData;
    context->initialBytes = task->downloadedBytes;
    if (context->curl == NULL || context->file == NULL) {
        if (context->curl != NULL)
            curl_easy_cleanup (context->curl);
        if (context->file != NULL)
            (void)fclose (context->file);
        c_free (context);
        errno = EIO;
        return -1;
    }
    (void)curl_easy_setopt (context->curl, CURLOPT_URL, task->resourceUrl);
    (void)curl_easy_setopt (context->curl, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt (context->curl, CURLOPT_WRITEFUNCTION, http_write);
    (void)curl_easy_setopt (context->curl, CURLOPT_WRITEDATA, context);
    (void)curl_easy_setopt (context->curl, CURLOPT_XFERINFOFUNCTION, http_progress);
    (void)curl_easy_setopt (context->curl, CURLOPT_XFERINFODATA, context);
    (void)curl_easy_setopt (context->curl, CURLOPT_NOPROGRESS, 0L);
    if (task->downloadedBytes > 0)
        (void)curl_easy_setopt (context->curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)task->downloadedBytes);
    *result = context;
    return 0;
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
    (void)fclose (task->file);
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
