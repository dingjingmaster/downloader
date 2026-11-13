#define _GNU_SOURCE

#include "database.h"
#include "task-manager.h"
#include "protocol.h"

#include <clib/memory.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ProtocolTask
{
    DownloadTask source;
    ProtocolCallbacks callbacks;
    void* userData;
};

static bool fixture_can_handle (const char* url, const char* protocol)
{
    (void)url;
    return strcmp (protocol, "https") == 0;
}

static int fixture_open (ProtocolTask** result, const DownloadTask* source, const ProtocolCallbacks* callbacks, void* userData)
{
    *result = c_malloc0 (sizeof (**result));
    assert (*result != NULL);
    (*result)->source = *source;
    (*result)->callbacks = *callbacks;
    (*result)->userData = userData;
    return 0;
}

static int fixture_run (ProtocolTask* task)
{
    assert (task->source.maxRetries == 2 && task->source.connectTimeout == 7 && task->source.transferTimeout == 11);
    assert (task->callbacks.onMetadata (task->userData, "\"fixture-v1\"", "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
    assert (task->callbacks.onRetry (task->userData, 1) == 0);
    FILE* file = fopen (task->source.tempPath, "wb");
    assert (file != NULL && fputs ("data", file) >= 0 && fclose (file) == 0);
    task->callbacks.onProgress (task->userData, 4, 4, 0);
    return 0;
}

static void fixture_close (ProtocolTask* task)
{
    c_free (task);
}

static void test_transfer_policy (const char* databasePath, const char* directory)
{
    static const ProtocolOps ops = {.name = "fixture", .canHandle = fixture_can_handle,
        .open = fixture_open, .run = fixture_run, .close = fixture_close};
    Database* database;
    assert (protocol_register (&ops) == 0);
    assert (database_open (databasePath, &database) == 0);
    TaskManager* manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_set_transfer_options (manager, -1, 0, 0) == -1 && errno == EINVAL);
    assert (task_manager_set_transfer_options (manager, 2, 7, 11) == 0);
    DownloadTask task;
    assert (task_manager_create (manager, "https://fixture.invalid/data.bin", NULL, directory, NULL, NULL, &task) == 0);
    assert (task.maxRetries == 2);
    assert (task_manager_start (manager, task.uuid) == 0);
    /* Execute takes ownership, just as the scheduler worker does. */
    DownloadTask* owned = c_malloc (sizeof (*owned));
    assert (owned != NULL);
    *owned = task;
    assert (task_manager_execute (manager, owned) == 0);
    assert (task_manager_get (manager, task.uuid, &task) == 0);
    assert (task.state == DOWNLOAD_TASK_STATE_COMPLETED && task.retryCount == 1);
    assert (strcmp (task.etag, "\"fixture-v1\"") == 0);
    assert (strcmp (task.lastModified, "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
    assert (unlink (task.targetPath) == 0);
    assert (task_manager_delete (manager, task.uuid) == 0);
    task_manager_free (manager);
    database_close (database);
}

static void test_task_lifecycle (const char* databasePath, const char* directory)
{
    Database*    database;
    TaskManager* manager;
    DownloadTask task;
    DownloadTask loaded;
    DownloadTask* page = NULL;
    size_t       count = 0;
    size_t       total = 0;
    size_t       deleted = 0;

    assert (database_open (databasePath, &database) == 0);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_create (manager, "https://example.test/file.bin", "file.bin", directory, "https", "{}",
                                 &task) == 0);
    assert (task.state == DOWNLOAD_TASK_STATE_CREATED);
    assert (task.totalBytes == 0);
    assert (strcmp (task.extraJson, "{}") == 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (strcmp (loaded.targetPath, task.targetPath) == 0);
    assert (task_manager_list (manager, 1, 20, &page, &count, &total) == 0);
    assert (count == 1 && total == 1);
    assert (strcmp (page[0].uuid, task.uuid) == 0);
    c_free (page);
    assert (task_manager_start (manager, task.uuid) == 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_QUEUED);
    assert (task_manager_pause (manager, task.uuid) == 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_PAUSED);
    assert (task_manager_resume (manager, task.uuid) == 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_QUEUED);
    loaded.state = DOWNLOAD_TASK_STATE_COMPLETED;
    loaded.completedAt = 123;
    assert (database_update (database, &loaded) == 0);
    assert (task_manager_clear_completed (manager, &deleted) == 0);
    assert (deleted == 1);
    errno = 0;
    assert (task_manager_get (manager, task.uuid, &loaded) != 0);
    assert (errno == ENOENT);
    assert (task_manager_stop (manager) == 0);
    assert (task_manager_start (manager, task.uuid) != 0 && errno == ESHUTDOWN);
    task_manager_free (manager);
    database_close (database);
}

static void test_task_validation (const char* databasePath, const char* directory)
{
    Database*    database;
    TaskManager* manager;
    DownloadTask task;

    assert (database_open (databasePath, &database) == 0);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    errno = 0;
    assert (task_manager_create (manager, "https://example.test/file.bin", "../file.bin", directory, "https", NULL,
                                 &task) != 0);
    assert (errno == EINVAL);
    assert (task_manager_create (manager, "https://example.test/video.mpd?token=x", NULL, directory, NULL, NULL,
                                 &task) == 0);
    assert (strcmp (task.protocol, "dash") == 0);
    DownloadTask other;
    assert (task_manager_create (manager, "https://example.test/video.mpd?token=y", NULL, directory, NULL, NULL,
                                 &other) == 0);
    assert (strcmp (task.tempPath, other.tempPath) != 0);
    assert (task_manager_delete (manager, task.uuid) == 0);
    assert (task_manager_delete (manager, other.uuid) == 0);
    errno = 0;
    assert (task_manager_create (manager, "", "file.bin", directory, "https", NULL, &task) != 0);
    assert (errno == EINVAL);
    task_manager_free (manager);
    database_close (database);
}

int main (void)
{
    char databasePath[] = "/tmp/downloader-task-manager-XXXXXX";
    char directory[] = "/tmp/downloader-task-files-XXXXXX";
    int  descriptor;
    char* createdDirectory;

    descriptor = mkstemp (databasePath);
    assert (descriptor >= 0);
    assert (close (descriptor) == 0);
    assert (unlink (databasePath) == 0);
    createdDirectory = mkdtemp (directory);
    assert (createdDirectory != NULL);
    test_task_lifecycle (databasePath, createdDirectory);
    test_task_validation (databasePath, createdDirectory);
    test_transfer_policy (databasePath, createdDirectory);
    assert (unlink (databasePath) == 0);
    return 0;
}
