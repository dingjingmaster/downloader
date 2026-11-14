#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "smb1.h"

#include <clib/memory.h>
#include <sys/time.h>
#include <libsmbclient.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DOWNLOADER_SMB1_CHUNK 65536U
#define DOWNLOADER_SMB1_TIMEOUT_SECONDS 30

struct ProtocolTask
{
    ProtocolCallbacks callbacks;
    void*             userData;
    char              url[DOWNLOAD_TASK_URL_SIZE];
    char              tempPath[DOWNLOAD_TASK_PATH_SIZE];
    bool              pauseRequested;
    bool              cancelRequested;
    uint64_t          total;
    uint64_t          downloaded;
    int64_t           notifiedAt;
    uint8_t           buffer[DOWNLOADER_SMB1_CHUNK];
};

static int64_t smb1_now_ms (void)
{
    struct timespec now;

    if (clock_gettime (CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void smb1_progress (ProtocolTask* task)
{
    int64_t now = smb1_now_ms ();

    if (task->callbacks.onProgress != NULL && now - task->notifiedAt >= 100) {
        task->notifiedAt = now;
        task->callbacks.onProgress (task->userData, task->total, task->downloaded, 0);
    }
}

static bool smb1_can_handle (const char* url, const char* protocol)
{
    return url != NULL && protocol != NULL && strcmp (protocol, "smb1") == 0 &&
           strncmp (url, "smb1://", 7) == 0;
}

static int smb1_open (ProtocolTask** result, const DownloadTask* download, const ProtocolCallbacks* callbacks,
                      void* userData)
{
    ProtocolTask* task;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    if (download == NULL || !smb1_can_handle (download->resourceUrl, download->protocol) ||
        memchr (download->resourceUrl, '\0', sizeof (download->resourceUrl)) == NULL ||
        memchr (download->tempPath, '\0', sizeof (download->tempPath)) == NULL ||
        download->tempPath[0] == '\0' || strlen (download->resourceUrl) >= sizeof (task->url)) {
        errno = EINVAL;
        return -1;
    }
    task = (ProtocolTask*)c_malloc0 (sizeof (*task));
    if (task == NULL)
        return -1;
    snprintf (task->url, sizeof (task->url), "%s", download->resourceUrl);
    snprintf (task->tempPath, sizeof (task->tempPath), "%s", download->tempPath);
    if (callbacks != NULL)
        task->callbacks = *callbacks;
    task->userData = userData;
    *result = task;
    return 0;
}

static int smb1_open_temp (ProtocolTask* task)
{
    int         descriptor;
    struct stat status;
    int         error;

    descriptor = open (task->tempPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (descriptor < 0)
        return -1;
    if (fstat (descriptor, &status) != 0) {
        error = errno;
        goto failed;
    }
    if (!S_ISREG (status.st_mode) || status.st_nlink != 1) {
        error = !S_ISREG (status.st_mode) ? EINVAL : EMLINK;
        goto failed;
    }
    if (ftruncate (descriptor, 0) != 0) {
        error = errno;
        (void)close (descriptor);
        errno = error;
        return -1;
    }
    return descriptor;
failed:
    (void)close (descriptor);
    errno = error;
    return -1;
}

static int smb1_run (ProtocolTask* task)
{
    SMBCCTX*    context = NULL;
    SMBCFILE*   remote = NULL;
    FILE*       file = NULL;
    char        url[DOWNLOAD_TASK_URL_SIZE];
    struct stat status;
    int         descriptor;
    int         error = EIO;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->total = 0;
    task->downloaded = 0;
    task->pauseRequested = false;
    task->cancelRequested = false;
    smb1_progress (task);
    if (task->callbacks.onStarted != NULL)
        task->callbacks.onStarted (task->userData);
    if (strlen (task->url) >= 4)
        snprintf (url, sizeof (url), "smb%s", task->url + 4);
    else
        url[0] = '\0';
    if (url[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    context = smbc_new_context ();
    if (context == NULL) {
        errno = ENOMEM;
        return -1;
    }
    smbc_setTimeout (context, DOWNLOADER_SMB1_TIMEOUT_SECONDS);
    smbc_setOptionProtocols (context, "NT1", "NT1");
    smbc_setOptionNoAutoAnonymousLogin (context, 1);
    if (smbc_init_context (context) == NULL) {
        smbc_free_context (context, 1);
        errno = EIO;
        return -1;
    }
    if (smbc_getFunctionStat (context) (context, url, &status) == 0 && status.st_size > 0)
        task->total = (uint64_t)status.st_size;
    remote = smbc_getFunctionOpen (context) (context, url, O_RDONLY, 0);
    if (remote == NULL) {
        error = errno != 0 ? errno : EACCES;
        goto failed;
    }
    descriptor = smb1_open_temp (task);
    if (descriptor < 0) {
        error = errno;
        goto failed;
    }
    file = fdopen (descriptor, "r+b");
    if (file == NULL) {
        error = errno;
        (void)close (descriptor);
        goto failed;
    }
    for (;;) {
        ssize_t count;
        if (task->cancelRequested) {
            error = ECANCELED;
            goto failed;
        }
        if (task->pauseRequested) {
            error = ECANCELED;
            goto paused;
        }
        count = smbc_getFunctionRead (context) (context, remote, task->buffer, sizeof (task->buffer));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = errno != 0 ? errno : EIO;
            goto failed;
        }
        if (count == 0)
            break;
        if (fwrite (task->buffer, 1, (size_t)count, file) != (size_t)count) {
            error = errno != 0 ? errno : EIO;
            goto failed;
        }
        task->downloaded += (uint64_t)count;
        smb1_progress (task);
    }
    if (fflush (file) != 0) {
        error = errno != 0 ? errno : EIO;
        goto failed;
    }
    (void)smbc_getFunctionClose (context) (context, remote);
    remote = NULL;
    if (fclose (file) != 0) {
        file = NULL;
        error = errno != 0 ? errno : EIO;
        goto failed;
    }
    file = NULL;
    smbc_free_context (context, 1);
    if (task->callbacks.onCompleted != NULL)
        task->callbacks.onCompleted (task->userData);
    return 0;
paused:
    if (file != NULL) {
        (void)fclose (file);
        file = NULL;
    }
    if (remote != NULL)
        (void)smbc_getFunctionClose (context) (context, remote);
    smbc_free_context (context, 1);
    if (task->callbacks.onPaused != NULL)
        task->callbacks.onPaused (task->userData);
    return 1;
failed:
    if (file != NULL)
        (void)fclose (file);
    if (remote != NULL)
        (void)smbc_getFunctionClose (context) (context, remote);
    smbc_free_context (context, 1);
    if (task->callbacks.onFailed != NULL) {
        char message[DOWNLOAD_TASK_ERROR_SIZE];
        snprintf (message, sizeof (message), "SMB1 download failed: %s", strerror (error));
        task->callbacks.onFailed (task->userData, error, message);
    }
    errno = error;
    return -1;
}

static int smb1_pause (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = true;
    return 0;
}

static int smb1_resume (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->pauseRequested = false;
    return 0;
}

static int smb1_cancel (ProtocolTask* task)
{
    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    task->cancelRequested = true;
    return 0;
}

static void smb1_close (ProtocolTask* task)
{
    c_free (task);
}

static bool smb1_retryable (ProtocolTask* task, int errorCode, const char* message)
{
    (void)task;
    (void)message;
    return errorCode == EIO || errorCode == EAGAIN || errorCode == ETIMEDOUT || errorCode == ECONNRESET ||
           errorCode == ECONNREFUSED || errorCode == EHOSTUNREACH || errorCode == ENETUNREACH ||
           errorCode == ENETDOWN || errorCode == EPIPE || errorCode == ECONNABORTED;
}

const ProtocolOps* smb1_protocol_ops (void)
{
    static const ProtocolOps operations = {
        .name = "smb1", .canHandle = smb1_can_handle, .open = smb1_open,
        .run = smb1_run, .pause = smb1_pause, .resume = smb1_resume,
        .cancel = smb1_cancel, .close = smb1_close, .retryable = smb1_retryable
    };
    return &operations;
}
