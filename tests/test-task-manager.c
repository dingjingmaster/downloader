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
#include <sys/stat.h>
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

static DownloadTaskCheckpoint gObservedCheckpoint;
static uint64_t gCheckpointDurable;
static uint32_t gCheckpointVersion;
static int      gCheckpointResult;
static bool     gCheckpointCalled;
static int      gFixtureOutcome;
static uint64_t gObservedInitialBytes;
static uint64_t gObservedFileSize;

static void fixture_reset (void)
{
    memset (&gObservedCheckpoint, 0, sizeof (gObservedCheckpoint));
    gCheckpointDurable = 4;
    gCheckpointVersion = DOWNLOAD_TASK_CHECKPOINT_VERSION;
    gCheckpointResult = 0;
    gCheckpointCalled = false;
    gFixtureOutcome = 0;
    gObservedInitialBytes = 0;
    gObservedFileSize = 0;
}

static int fixture_run (ProtocolTask* task)
{
    struct stat status;
    assert (task->source.maxRetries == 2 && task->source.connectTimeout == 7 && task->source.transferTimeout == 11);
    gObservedInitialBytes = task->source.downloadedBytes;
    gObservedFileSize = stat (task->source.tempPath, &status) == 0 ? (uint64_t)status.st_size : 0;
    if (gFixtureOutcome == 1)
        return 1;
    assert (task->callbacks.onMetadata (task->userData, "\"fixture-v1\"", "Wed, 21 Oct 2015 07:28:00 GMT") == 0);
    assert (task->callbacks.onRetry (task->userData, 1) == 0);
    FILE* file = fopen (task->source.tempPath, "wb");
    assert (file != NULL && fputs ("data", file) >= 0 && fflush (file) == 0 && fsync (fileno (file)) == 0 &&
            fclose (file) == 0);
    task->callbacks.onProgress (task->userData, 4, 4, 0);
    memset (&gObservedCheckpoint, 0, sizeof (gObservedCheckpoint));
    gObservedCheckpoint.version = gCheckpointVersion;
    gObservedCheckpoint.durableBytes = gCheckpointDurable;
    snprintf (gObservedCheckpoint.effectiveUrl, sizeof (gObservedCheckpoint.effectiveUrl),
              "https://fixture.invalid/final/data.bin");
    snprintf (gObservedCheckpoint.etag, sizeof (gObservedCheckpoint.etag), "\"fixture-v1\"");
    snprintf (gObservedCheckpoint.lastModified, sizeof (gObservedCheckpoint.lastModified),
              "Wed, 21 Oct 2015 07:28:00 GMT");
    snprintf (gObservedCheckpoint.requestContext, sizeof (gObservedCheckpoint.requestContext), "fnv1a64:fixture");
    assert (task->callbacks.onCheckpoint != NULL);
    gCheckpointCalled = true;
    gCheckpointResult = task->callbacks.onCheckpoint (task->userData, &gObservedCheckpoint);
    if (gCheckpointResult != 0)
        return -1;
    /* Mode 2 models a protocol that persisted a checkpoint and then observed
     * the core's pause request before declaring completion. */
    return gFixtureOutcome == 2 ? 1 : 0;
}

static void fixture_close (ProtocolTask* task)
{
    c_free (task);
}

static void test_transfer_policy (const char* databasePath, const char* directory)
{
    static const ProtocolOps ops = {.name = "fixture", .canHandle = fixture_can_handle,
        .open = fixture_open, .run = fixture_run, .close = fixture_close};
    fixture_reset ();
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

static void execute_owned (TaskManager* manager, const DownloadTask* task)
{
    DownloadTask* owned = c_malloc (sizeof (*owned));

    assert (owned != NULL);
    *owned = *task;
    (void)task_manager_execute (manager, owned);
}

static DownloadTaskCheckpoint fixture_checkpoint (uint64_t durableBytes)
{
    DownloadTaskCheckpoint checkpoint;

    memset (&checkpoint, 0, sizeof (checkpoint));
    checkpoint.version = DOWNLOAD_TASK_CHECKPOINT_VERSION;
    checkpoint.durableBytes = durableBytes;
    snprintf (checkpoint.effectiveUrl, sizeof (checkpoint.effectiveUrl), "https://fixture.invalid/final/data.bin");
    snprintf (checkpoint.etag, sizeof (checkpoint.etag), "\"fixture-v1\"");
    snprintf (checkpoint.lastModified, sizeof (checkpoint.lastModified), "Wed, 21 Oct 2015 07:28:00 GMT");
    snprintf (checkpoint.requestContext, sizeof (checkpoint.requestContext), "fnv1a64:fixture");
    return checkpoint;
}

static void test_checkpoint_pause_and_completion (const char* databasePath, const char* directory)
{
    Database*              database;
    TaskManager*           manager;
    DownloadTask           task;
    DownloadTask           loaded;
    DownloadTaskCheckpoint checkpoint;

    fixture_reset ();
    gFixtureOutcome = 2;
    assert (database_open (databasePath, &database) == 0);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_set_transfer_options (manager, 2, 7, 11) == 0);
    assert (task_manager_create (manager, "https://fixture.invalid/checkpoint.bin", "checkpoint.bin", directory,
                                 "https", NULL, &task) == 0);
    assert (task_manager_start (manager, task.uuid) == 0);
    execute_owned (manager, &task);
    assert (gCheckpointCalled && gCheckpointResult == 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_PAUSED);
    assert (strcmp (loaded.etag, "\"fixture-v1\"") == 0);
    assert (database_load_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (checkpoint.version == DOWNLOAD_TASK_CHECKPOINT_VERSION);
    assert (checkpoint.durableBytes == 4);
    assert (strcmp (checkpoint.effectiveUrl, "https://fixture.invalid/final/data.bin") == 0);
    assert (strcmp (checkpoint.etag, "\"fixture-v1\"") == 0);
    fixture_reset ();
    assert (task_manager_start (manager, task.uuid) == 0);
    execute_owned (manager, &loaded);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_COMPLETED);
    /* A committed task cannot resume; its checkpoint must not linger. */
    assert (database_load_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (checkpoint.version == 0);
    assert (unlink (loaded.targetPath) == 0);
    assert (task_manager_delete (manager, task.uuid) == 0);
    task_manager_free (manager);
    database_close (database);
}

static void test_checkpoint_trims_unverified_tail (const char* databasePath, const char* directory)
{
    Database*              database;
    TaskManager*           manager;
    DownloadTask           task;
    DownloadTask           loaded;
    DownloadTaskCheckpoint checkpoint;
    FILE*                  file;

    fixture_reset ();
    gFixtureOutcome = 1;
    assert (database_open (databasePath, &database) == 0);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_set_transfer_options (manager, 2, 7, 11) == 0);
    assert (task_manager_create (manager, "https://fixture.invalid/trim.bin", "trim.bin", directory, "https", NULL,
                                 &task) == 0);
    file = fopen (task.tempPath, "wb");
    assert (file != NULL && fputs ("data", file) >= 0 && fclose (file) == 0);
    checkpoint = fixture_checkpoint (2);
    assert (database_save_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (task_manager_start (manager, task.uuid) == 0);
    execute_owned (manager, &task);
    /* Bytes past the durable checkpoint were written but never proven durable. */
    assert (gObservedInitialBytes == 2);
    assert (gObservedFileSize == 2);
    assert (database_load_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (checkpoint.durableBytes == 2);
    checkpoint = fixture_checkpoint (10);
    assert (database_save_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (task_manager_start (manager, task.uuid) == 0);
    execute_owned (manager, &task);
    /* The file is shorter than the checkpoint claims: restart from zero. */
    assert (gObservedInitialBytes == 0);
    assert (gObservedFileSize == 0);
    assert (database_load_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (checkpoint.version == 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.etag[0] == '\0');
    assert (loaded.lastModified[0] == '\0');
    assert (task_manager_delete (manager, task.uuid) == 0);
    task_manager_free (manager);
    database_close (database);
}

static void checkpoint_failure (const char* databasePath, const char* directory, bool unknownVersion)
{
    Database*              database;
    TaskManager*           manager;
    DownloadTask           task;
    DownloadTask           loaded;
    DownloadTaskCheckpoint checkpoint;

    fixture_reset ();
    if (unknownVersion)
        gCheckpointVersion = DOWNLOAD_TASK_CHECKPOINT_VERSION + 1;
    else
        gCheckpointDurable = 3;
    assert (database_open (databasePath, &database) == 0);
    manager = task_manager_new (database, NULL);
    assert (manager != NULL);
    assert (task_manager_set_transfer_options (manager, 2, 7, 11) == 0);
    assert (task_manager_create (manager, "https://fixture.invalid/reject.bin", "reject.bin", directory, "https", NULL,
                                 &task) == 0);
    assert (task_manager_start (manager, task.uuid) == 0);
    execute_owned (manager, &task);
    assert (gCheckpointResult != 0);
    assert (task_manager_get (manager, task.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_FAILED);
    assert (database_load_checkpoint (database, task.uuid, &checkpoint) == 0);
    assert (checkpoint.version == 0);
    assert (task_manager_delete (manager, task.uuid) == 0);
    task_manager_free (manager);
    database_close (database);
}

static void test_checkpoint_rejects_durable_mismatch (const char* databasePath, const char* directory)
{
    checkpoint_failure (databasePath, directory, false);
}

static void test_checkpoint_rejects_unknown_version (const char* databasePath, const char* directory)
{
    checkpoint_failure (databasePath, directory, true);
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
    test_checkpoint_pause_and_completion (databasePath, createdDirectory);
    test_checkpoint_trims_unverified_tail (databasePath, createdDirectory);
    test_checkpoint_rejects_durable_mismatch (databasePath, createdDirectory);
    test_checkpoint_rejects_unknown_version (databasePath, createdDirectory);
    assert (unlink (databasePath) == 0);
    return 0;
}
