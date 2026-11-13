#define _POSIX_C_SOURCE 200809L

#include "task-manager.h"

#include "filesystem.h"
#include "protocol.h"
#include <sys/stat.h>

#include <clib/memory.h>
#include <clib/mutex.h>
#include <clib/cond.h>
#include <clib/uri.h>
#include <clib/directory.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct ActiveTask ActiveTask;
struct ActiveTask
{
    DownloadTask task;
    ActiveTask* next;
};

struct TaskManager
{
    Database* database;
    Scheduler* scheduler;
    CMutex    mutex;
    uint64_t  sequence;
    ActiveTask* active;
    CCond idle;
    char defaultDirectory[DOWNLOAD_TASK_PATH_SIZE];
    bool stopping;
};

int task_manager_set_download_directory (TaskManager* manager, const char* directory)
{
    if (manager == NULL || directory == NULL || !directory[0] || strlen (directory) >= sizeof (manager->defaultDirectory)) {
        errno = EINVAL;
        return -1;
    }
    snprintf (manager->defaultDirectory, sizeof (manager->defaultDirectory), "%s", directory);
    return 0;
}

static bool task_manager_active (TaskManager* manager, const char* uuid)
{
    for (ActiveTask* active = manager->active; active != NULL; active = active->next) {
        if (strcmp (active->task.uuid, uuid) == 0)
            return true;
    }
    return false;
}

const char* task_manager_download_directory (const TaskManager* manager)
{
    return manager == NULL ? NULL : manager->defaultDirectory;
}

static void task_manager_release (TaskManager* manager, DownloadTask* task)
{
    ActiveTask** slot = &manager->active;
    while (*slot != NULL && &(*slot)->task != task)
        slot = &(*slot)->next;
    if (*slot != NULL)
        *slot = (*slot)->next;
    c_free (task);
    c_cond_notify_all (&manager->idle);
}

static bool task_manager_has_parent_reference (const char* value)
{
    return value != NULL && (strstr (value, "../") != NULL || strstr (value, "/..") != NULL || strcmp (value, "..") == 0);
}

static int task_manager_validate (const char* resourceUrl, const char* fileName, const char* targetDirectory,
                                  const char* protocol)
{
    if (resourceUrl == NULL || resourceUrl[0] == '\0' || fileName == NULL || fileName[0] == '\0' ||
        targetDirectory == NULL || targetDirectory[0] == '\0' || protocol == NULL || protocol[0] == '\0' ||
        strchr (fileName, '/') != NULL || strchr (fileName, '\\') != NULL || task_manager_has_parent_reference (fileName) ||
        task_manager_has_parent_reference (targetDirectory)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int64_t task_manager_now (void)
{
    return (int64_t)time (NULL);
}

static int task_manager_update_state (TaskManager* manager, const char* uuid, DownloadTaskState state)
{
    DownloadTask task;
    int          result;

    if (database_load (manager->database, uuid, &task) != 0)
        return -1;
    if (state == DOWNLOAD_TASK_STATE_QUEUED && task.state == DOWNLOAD_TASK_STATE_CREATED)
        task.startedAt = task_manager_now ();
    if (state == DOWNLOAD_TASK_STATE_PAUSED)
        task.pausedAt = task_manager_now ();
    task.updatedAt = task_manager_now ();
    task.state = state;
    result = database_update (manager->database, &task);
    return result;
}

TaskManager* task_manager_new (Database* database, Scheduler* scheduler)
{
    TaskManager* manager;

    if (database == NULL) {
        errno = EINVAL;
        return NULL;
    }
    manager = (TaskManager*)c_malloc0 (sizeof (*manager));
    if (manager == NULL)
        return NULL;
    if (!c_mutex_init (&manager->mutex)) {
        c_free (manager);
        errno = ENOMEM;
        return NULL;
    }
    manager->database = database;
    strcpy (manager->defaultDirectory, ".");
    if (!c_cond_init (&manager->idle)) {
        c_mutex_clear (&manager->mutex);
        c_free (manager);
        return NULL;
    }
    manager->scheduler = scheduler;
    return manager;
}

void task_manager_free (TaskManager* manager)
{
    if (manager == NULL)
        return;
    c_mutex_clear (&manager->mutex);
    c_cond_clear (&manager->idle);
    c_free (manager);
}

int task_manager_create (TaskManager* manager, const char* resourceUrl, const char* fileName, const char* targetDirectory,
                         const char* protocol, const char* extraJson, DownloadTask* result)
{
    DownloadTask task;
    int64_t      now;
    CUri uri;
    size_t uriError;
    char inferredProtocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char inferredName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char* decoded = NULL;
    size_t decodedSize;

    if (manager == NULL || resourceUrl == NULL ||
        c_uri_parse (c_string_view_from_cstr (resourceUrl), &uri, &uriError) != 0 ||
        uri.scheme.size == 0 || uri.scheme.size >= sizeof (inferredProtocol)) {
        errno = EINVAL;
        return -1;
    }
    if (protocol_detect (resourceUrl, inferredProtocol, sizeof (inferredProtocol)) != 0)
        return -1;
    protocol = inferredProtocol;
    if (targetDirectory == NULL || !targetDirectory[0])
        targetDirectory = manager->defaultDirectory;
    if (fileName == NULL || !fileName[0]) {
        CStringView basename = uri.path;
        for (size_t i = 0; i < uri.path.size; i++) {
            if (uri.path.data[i] == '/') {
                basename.data = uri.path.data + i + 1;
                basename.size = uri.path.size - i - 1;
            }
        }
        if (c_uri_unescape_string (basename, &decoded, &decodedSize) != 0)
            return -1;
        if (decodedSize >= sizeof (inferredName) || memchr (decoded, '\0', decodedSize) != NULL) {
            c_free (decoded);
            errno = EINVAL;
            return -1;
        }
        snprintf (inferredName, sizeof (inferredName), "%s", decodedSize ? decoded : "download.bin");
        c_free (decoded);
        fileName = inferredName;
    }

    if (manager == NULL || result == NULL || task_manager_validate (resourceUrl, fileName, targetDirectory, protocol) != 0)
        return -1;
    if (strlen (resourceUrl) >= sizeof (task.resourceUrl) || strlen (fileName) >= sizeof (task.fileName) ||
        strlen (targetDirectory) >= sizeof (task.targetDirectory) ||
        (extraJson != NULL && strlen (extraJson) >= sizeof (task.extraJson))) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset (&task, 0, sizeof (task));
    now = task_manager_now ();
    c_mutex_lock (&manager->mutex);
    manager->sequence++;
    (void)snprintf (task.uuid, sizeof (task.uuid), "task-%lld-%ld-%llu", (long long)now, (long)getpid(),
                    (unsigned long long)manager->sequence);
    (void)snprintf (task.fileName, sizeof (task.fileName), "%s", fileName);
    (void)snprintf (task.targetDirectory, sizeof (task.targetDirectory), "%s", targetDirectory);
    (void)snprintf (task.resourceUrl, sizeof (task.resourceUrl), "%s", resourceUrl);
    (void)snprintf (task.protocol, sizeof (task.protocol), "%s", protocol);
    (void)snprintf (task.extraJson, sizeof (task.extraJson), "%s", extraJson == NULL ? "{}" : extraJson);
    task.state = DOWNLOAD_TASK_STATE_CREATED;
    task.createdAt = now;
    task.updatedAt = now;
    task.maxRetries = 3;
    if (filesystem_prepare (&task) != 0 || database_insert (manager->database, &task) != 0) {
        int savedErrno = errno;
        c_mutex_unlock (&manager->mutex);
        errno = savedErrno;
        return -1;
    }
    *result = task;
    c_mutex_unlock (&manager->mutex);
    return 0;
}

int task_manager_delete (TaskManager* manager, const char* uuid)
{
    DownloadTask task;
    int          result;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, uuid, &task) != 0) {
        result = -1;
    } else {
        if (task_manager_active (manager, uuid)) {
            if (task_manager_update_state (manager, uuid, DOWNLOAD_TASK_STATE_CANCELLED) != 0) {
                c_mutex_unlock (&manager->mutex);
                return -1;
            }
            while (task_manager_active (manager, uuid))
                c_cond_wait (&manager->idle, &manager->mutex);
        }
        result = filesystem_remove_partial (&task);
        if (result == 0)
            result = database_delete (manager->database, uuid);
    }
    {
        int savedErrno = errno;
        c_mutex_unlock (&manager->mutex);
        errno = savedErrno;
    }
    return result;
}

int task_manager_start (TaskManager* manager, const char* uuid)
{
    int result;
    DownloadTask* queued;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    if (manager->stopping) {
        c_mutex_unlock (&manager->mutex);
        errno = ESHUTDOWN;
        return -1;
    }
    if (task_manager_active (manager, uuid)) {
        c_mutex_unlock (&manager->mutex);
        errno = EBUSY;
        return -1;
    }
    queued = c_malloc0 (sizeof (ActiveTask));
    if (queued == NULL || database_load (manager->database, uuid, queued) != 0) {
        c_free (queued);
        c_mutex_unlock (&manager->mutex);
        return -1;
    }
    if (queued->state == DOWNLOAD_TASK_STATE_DOWNLOADING || queued->state == DOWNLOAD_TASK_STATE_COMPLETED) {
        c_free (queued);
        c_mutex_unlock (&manager->mutex);
        errno = EBUSY;
        return -1;
    }
    result = task_manager_update_state (manager, uuid, DOWNLOAD_TASK_STATE_QUEUED);
    if (result == 0 && manager->scheduler != NULL) {
        ((ActiveTask*)queued)->next = manager->active;
        manager->active = (ActiveTask*)queued;
        result = scheduler_submit (manager->scheduler, queued);
        if (result != 0) {
            manager->active = ((ActiveTask*)queued)->next;
            database_update (manager->database, queued);
        }
    }
    if (result != 0 || manager->scheduler == NULL)
        c_free (queued);
    c_mutex_unlock (&manager->mutex);
    return result;
}

typedef struct
{
    TaskManager* manager;
    DownloadTask* task;
    ProtocolTask* context;
    const ProtocolOps* ops;
    int errorCode;
    char errorMessage[DOWNLOAD_TASK_ERROR_SIZE];
} DownloadExecution;

static void task_manager_failed (void* userData, int code, const char* message)
{
    DownloadExecution* execution = userData;
    execution->errorCode = code;
    snprintf (execution->errorMessage, sizeof (execution->errorMessage), "%s", message == NULL ? "" : message);
}

static void task_manager_progress (void* userData, uint64_t total, uint64_t downloaded, uint64_t speed)
{
    DownloadExecution* execution = userData;
    DownloadTask current;
    c_mutex_lock (&execution->manager->mutex);
    if (database_load (execution->manager->database, execution->task->uuid, &current) != 0) {
        execution->ops->cancel (execution->context);
    } else if (current.state == DOWNLOAD_TASK_STATE_CANCELLED) {
        execution->ops->cancel (execution->context);
    } else if (current.state == DOWNLOAD_TASK_STATE_PAUSED || execution->manager->stopping) {
        execution->ops->pause (execution->context);
    } else {
        current.totalBytes = total;
        current.downloadedBytes = downloaded;
        current.speedBytesSec = speed;
        current.progress = total == 0 ? 0 : 100.0 * (downloaded > total ? total : downloaded) / total;
        current.updatedAt = task_manager_now ();
        if (database_update (execution->manager->database, &current) != 0)
            execution->ops->cancel (execution->context);
    }
    c_mutex_unlock (&execution->manager->mutex);
}

static void task_manager_filename (void* userData, const char* name)
{
    DownloadExecution* execution = userData;
    DownloadTask current;
    if (!name[0] || strlen (name) >= sizeof (current.fileName) || strchr (name, '/') || strchr (name, '\\') ||
        strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
        return;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        if (*p < 32 || *p == 127)
            return;
    }
    c_mutex_lock (&execution->manager->mutex);
    if (database_load (execution->manager->database, execution->task->uuid, &current) == 0 &&
        snprintf (current.targetPath, sizeof (current.targetPath), "%s/%s", current.targetDirectory, name) < (int)sizeof (current.targetPath)) {
        strcpy (current.fileName, name);
        if (database_update (execution->manager->database, &current) == 0) {
            strcpy (execution->task->fileName, current.fileName);
            strcpy (execution->task->targetPath, current.targetPath);
        }
    }
    c_mutex_unlock (&execution->manager->mutex);
}

int task_manager_execute (TaskManager* manager, DownloadTask* task)
{
    DownloadExecution execution = { .manager = manager, .task = task };
    ProtocolCallbacks callbacks = { .onProgress = task_manager_progress, .onFileName = task_manager_filename,
                                    .onFailed = task_manager_failed };
    DownloadTask current;
    struct stat status;
    int result = -1;
    int savedErrno;
    bool publishedWarning = false;
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, task->uuid, &current) != 0 || current.state != DOWNLOAD_TASK_STATE_QUEUED) {
        task_manager_release (manager, task);
        c_mutex_unlock (&manager->mutex);
        return -1;
    }
    *task = current;
    if (stat (task->tempPath, &status) == 0 && S_ISREG (status.st_mode))
        task->downloadedBytes = (uint64_t)status.st_size;
    task->state = DOWNLOAD_TASK_STATE_DOWNLOADING;
    task->startedAt = task_manager_now ();
    task->errorCode = 0;
    task->errorMessage[0] = '\0';
    result = database_update (manager->database, task);
    c_mutex_unlock (&manager->mutex);
    execution.ops = protocol_find (task->resourceUrl, task->protocol);
    if (result == 0 && execution.ops != NULL &&
        execution.ops->open (&execution.context, task, &callbacks, &execution) == 0) {
        result = execution.ops->run (execution.context);
        savedErrno = errno;
        execution.ops->close (execution.context);
        errno = savedErrno;
    } else {
        result = -1;
    }
    savedErrno = errno;
    c_mutex_lock (&manager->mutex);
    if (database_load (manager->database, task->uuid, &current) == 0) {
        if (stat (task->tempPath, &status) == 0 && S_ISREG (status.st_mode))
            current.downloadedBytes = (uint64_t)status.st_size;
        if (result == 1 && current.state == DOWNLOAD_TASK_STATE_DOWNLOADING) {
            current.state = DOWNLOAD_TASK_STATE_PAUSED;
            current.pausedAt = task_manager_now ();
        }
        if (current.state != DOWNLOAD_TASK_STATE_PAUSED && current.state != DOWNLOAD_TASK_STATE_CANCELLED) {
            if (result == 0) {
                result = filesystem_commit (task);
                savedErrno = errno;
                if (result == 1) {
                    publishedWarning = true;
                    result = 0;
                }
            }
            current.state = result == 0 ? DOWNLOAD_TASK_STATE_COMPLETED : DOWNLOAD_TASK_STATE_FAILED;
            current.errorCode = result == 0 ? 0 : (savedErrno == 0 ? EIO : savedErrno);
            snprintf (current.errorMessage, sizeof (current.errorMessage), "%s", result == 0 ? "" :
                      (execution.errorMessage[0] ? execution.errorMessage : strerror (current.errorCode)));
            if (publishedWarning) {
                current.errorCode = savedErrno;
                snprintf (current.errorMessage, sizeof (current.errorMessage),
                          "Output published; parent directory sync failed: %s", strerror (savedErrno));
            }
            if (result == 0) {
                current.completedAt = task_manager_now ();
                current.progress = 100;
                if (current.totalBytes == 0)
                    current.totalBytes = current.downloadedBytes;
            }
        }
        current.speedBytesSec = 0;
        result = database_update (manager->database, &current);
    }
    task_manager_release (manager, task);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_stop (TaskManager* manager)
{
    int savedErrno = 0;
    if (manager == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    manager->stopping = true;
    for (ActiveTask* active = manager->active; active != NULL; active = active->next) {
        if (task_manager_update_state (manager, active->task.uuid, DOWNLOAD_TASK_STATE_PAUSED) != 0)
            savedErrno = errno;
    }
    c_mutex_unlock (&manager->mutex);
    errno = savedErrno;
    return savedErrno == 0 ? 0 : -1;
}

int task_manager_pause (TaskManager* manager, const char* uuid)
{
    int result;

    if (manager == NULL || uuid == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    DownloadTask current;
    if (database_load (manager->database, uuid, &current) != 0 ||
        current.state == DOWNLOAD_TASK_STATE_COMPLETED || current.state == DOWNLOAD_TASK_STATE_CANCELLED) {
        c_mutex_unlock (&manager->mutex);
        errno = EINVAL;
        return -1;
    }
    result = task_manager_update_state (manager, uuid, DOWNLOAD_TASK_STATE_PAUSED);
    while (result == 0 && task_manager_active (manager, uuid))
        c_cond_wait (&manager->idle, &manager->mutex);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_resume (TaskManager* manager, const char* uuid)
{
    return task_manager_start (manager, uuid);
}

int task_manager_get (TaskManager* manager, const char* uuid, DownloadTask* task)
{
    int result;

    if (manager == NULL || uuid == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    result = database_load (manager->database, uuid, task);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_list (TaskManager* manager, size_t page, size_t pageSize, DownloadTask** tasks, size_t* count,
                       size_t* total)
{
    size_t offset;
    int    result;

    if (manager == NULL || page == 0 || pageSize == 0 || page > SIZE_MAX / pageSize || tasks == NULL || count == NULL ||
        total == NULL) {
        errno = EINVAL;
        return -1;
    }
    offset = (page - 1U) * pageSize;
    c_mutex_lock (&manager->mutex);
    result = database_list (manager->database, offset, pageSize, tasks, count, total);
    c_mutex_unlock (&manager->mutex);
    return result;
}

int task_manager_clear_completed (TaskManager* manager, size_t* deleted)
{
    int result;

    if (manager == NULL || deleted == NULL) {
        errno = EINVAL;
        return -1;
    }
    c_mutex_lock (&manager->mutex);
    result = database_clear_completed (manager->database);
    if (result >= 0)
        *deleted = (size_t)result;
    c_mutex_unlock (&manager->mutex);
    return result < 0 ? -1 : 0;
}
