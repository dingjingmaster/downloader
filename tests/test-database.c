#define _GNU_SOURCE

#include "database.h"
#include "task.h"

#include <clib/memory.h>

#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Schema created by downloader before checkpoint support existed. */
static const char gcLegacySchema[] =
    "CREATE TABLE download_tasks ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "uuid TEXT NOT NULL UNIQUE,"
    "file_name TEXT NOT NULL,"
    "target_directory TEXT NOT NULL,"
    "target_path TEXT NOT NULL,"
    "temp_path TEXT NOT NULL,"
    "resource_url TEXT NOT NULL,"
    "protocol TEXT NOT NULL,"
    "state TEXT NOT NULL,"
    "error_code INTEGER NOT NULL DEFAULT 0,"
    "error_message TEXT NOT NULL DEFAULT '',"
    "total_bytes INTEGER NOT NULL DEFAULT 0,"
    "downloaded_bytes INTEGER NOT NULL DEFAULT 0,"
    "progress REAL NOT NULL DEFAULT 0,"
    "speed_bytes_sec INTEGER NOT NULL DEFAULT 0,"
    "created_at INTEGER NOT NULL DEFAULT 0,"
    "started_at INTEGER NOT NULL DEFAULT 0,"
    "paused_at INTEGER NOT NULL DEFAULT 0,"
    "completed_at INTEGER NOT NULL DEFAULT 0,"
    "updated_at INTEGER NOT NULL DEFAULT 0,"
    "retry_count INTEGER NOT NULL DEFAULT 0,"
    "max_retries INTEGER NOT NULL DEFAULT 3,"
    "checksum_type TEXT NOT NULL DEFAULT '',"
    "checksum_value TEXT NOT NULL DEFAULT '',"
    "etag TEXT NOT NULL DEFAULT '',"
    "last_modified TEXT NOT NULL DEFAULT '',"
    "extra_json TEXT NOT NULL DEFAULT '{}'"
    ");";

static const char gcLegacyRow[] =
    "INSERT INTO download_tasks (uuid,file_name,target_directory,target_path,temp_path,resource_url,protocol,state,"
    "downloaded_bytes,total_bytes,etag,last_modified) VALUES "
    "('legacy-1','legacy.bin','/tmp','/tmp/legacy.bin','/tmp/.legacy-1.part','https://example.test/legacy.bin',"
    "'https','paused',512,4096,'\"legacy-etag\"','Wed, 21 Oct 2015 07:28:00 GMT');";

static DownloadTask make_task (const char* uuid, DownloadTaskState state)
{
    DownloadTask task;

    memset (&task, 0, sizeof (task));
    snprintf (task.uuid, sizeof (task.uuid), "%s", uuid);
    snprintf (task.fileName, sizeof (task.fileName), "sample.bin");
    snprintf (task.targetDirectory, sizeof (task.targetDirectory), "/tmp/downloads");
    snprintf (task.targetPath, sizeof (task.targetPath), "/tmp/downloads/sample.bin");
    snprintf (task.tempPath, sizeof (task.tempPath), "/tmp/downloads/sample.bin.part");
    snprintf (task.resourceUrl, sizeof (task.resourceUrl), "https://example.test/sample.bin");
    snprintf (task.protocol, sizeof (task.protocol), "https");
    snprintf (task.extraJson, sizeof (task.extraJson), "{\"source\":\"test\"}");
    task.state = state;
    task.totalBytes = 1000;
    task.downloadedBytes = 250;
    task.progress = 25.0;
    task.createdAt = 100;
    task.updatedAt = 100;
    task.maxRetries = 3;
    return task;
}

static void test_insert_and_load (const char* path)
{
    Database*    database;
    DownloadTask expected = make_task ("task-1", DOWNLOAD_TASK_STATE_QUEUED);
    DownloadTask actual;

    assert (database_open (path, &database) == 0);
    assert (database_insert (database, &expected) == 0);
    memset (&actual, 0, sizeof (actual));
    assert (database_load (database, expected.uuid, &actual) == 0);
    assert (strcmp (actual.uuid, expected.uuid) == 0);
    assert (strcmp (actual.fileName, expected.fileName) == 0);
    assert (actual.state == DOWNLOAD_TASK_STATE_QUEUED);
    assert (actual.totalBytes == 1000);
    assert (actual.downloadedBytes == 250);
    assert (actual.progress == 25.0);
    assert (strcmp (actual.extraJson, expected.extraJson) == 0);
    assert (download_task_set_state (&actual, DOWNLOAD_TASK_STATE_DOWNLOADING) == 0);
    actual.downloadedBytes = 500;
    actual.durationMillis = 4200;
    actual.gapCount = 2;
    assert (database_update (database, &actual) == 0);
    memset (&actual, 0, sizeof (actual));
    assert (database_load (database, expected.uuid, &actual) == 0);
    assert (actual.state == DOWNLOAD_TASK_STATE_DOWNLOADING);
    assert (actual.downloadedBytes == 500);
    assert (actual.durationMillis == 4200);
    assert (actual.gapCount == 2);
    database_close (database);
}

static void test_recovery_and_clear (const char* path)
{
    Database*    database;
    DownloadTask running = make_task ("task-running", DOWNLOAD_TASK_STATE_DOWNLOADING);
    DownloadTask completed = make_task ("task-completed", DOWNLOAD_TASK_STATE_COMPLETED);
    DownloadTask loaded;

    assert (database_open (path, &database) == 0);
    assert (database_insert (database, &running) == 0);
    assert (database_insert (database, &completed) == 0);
    assert (database_recover (database) == 0);
    assert (database_load (database, running.uuid, &loaded) == 0);
    assert (loaded.state == DOWNLOAD_TASK_STATE_QUEUED);
    assert (database_clear_completed (database) == 1);
    assert (database_load (database, completed.uuid, &loaded) != 0);
    database_close (database);
}

static DownloadTaskCheckpoint make_checkpoint (uint64_t durableBytes)
{
    DownloadTaskCheckpoint checkpoint;

    memset (&checkpoint, 0, sizeof (checkpoint));
    checkpoint.version = DOWNLOAD_TASK_CHECKPOINT_VERSION;
    checkpoint.durableBytes = durableBytes;
    snprintf (checkpoint.effectiveUrl, sizeof (checkpoint.effectiveUrl),
              "https://cdn.example.test/final/sample.bin?token=stable");
    snprintf (checkpoint.etag, sizeof (checkpoint.etag), "\"checkpoint-v1\"");
    snprintf (checkpoint.lastModified, sizeof (checkpoint.lastModified), "Wed, 21 Oct 2015 07:28:00 GMT");
    snprintf (checkpoint.requestContext, sizeof (checkpoint.requestContext), "fnv1a64:0123456789abcdef");
    return checkpoint;
}

static void test_group_fields (const char* path)
{
    Database* database;
    DownloadTask tasks[3];
    DownloadTask* listed = NULL;
    size_t count = 0;
    DownloadTaskGroupSummary summary;

    assert (database_open (path, &database) == 0);
    for (size_t index = 0; index < 3; index++) {
        tasks[index] = make_task (index == 0 ? "group-a" : index == 1 ? "group-b" : "group-c",
                                  index == 0 ? DOWNLOAD_TASK_STATE_COMPLETED
                                             : index == 1 ? DOWNLOAD_TASK_STATE_FAILED
                                                          : DOWNLOAD_TASK_STATE_QUEUED);
        snprintf (tasks[index].groupUuid, sizeof (tasks[index].groupUuid), "group-1");
        snprintf (tasks[index].groupName, sizeof (tasks[index].groupName), "资料");
        tasks[index].totalBytes = 100;
        tasks[index].downloadedBytes = index == 0 ? 100 : (index == 1 ? 25 : 10);
    }
    assert (database_insert_many (database, tasks, 3) == 0);
    assert (database_list_group (database, "group-1", &listed, &count) == 0);
    assert (count == 3);
    assert (strcmp (listed[0].groupUuid, "group-1") == 0);
    assert (strcmp (listed[0].groupName, "资料") == 0);
    c_free (listed);
    listed = NULL;
    assert (database_group_summary (database, "group-1", &summary) == 0);
    assert (summary.taskCount == 3);
    assert (summary.completedCount == 1);
    assert (summary.failedCount == 1);
    assert (summary.totalBytes == 300);
    assert (summary.downloadedBytes == 135);
    /* 批量插入必须原子：重复 uuid 时整批回滚。 */
    tasks[2].uuid[0] = '\0';
    snprintf (tasks[2].uuid, sizeof (tasks[2].uuid), "%s", tasks[0].uuid);
    assert (database_insert_many (database, tasks, 3) != 0);
    assert (database_list_group (database, "group-1", &listed, &count) == 0);
    assert (count == 3);
    c_free (listed);
    database_close (database);
}

static void test_checkpoint_persistence (const char* path)
{
    Database*              database;
    DownloadTask           task = make_task ("task-checkpoint", DOWNLOAD_TASK_STATE_PAUSED);
    DownloadTaskCheckpoint expected = make_checkpoint (4096);
    DownloadTaskCheckpoint actual;
    int                    version = 0;

    assert (database_open (path, &database) == 0);
    assert (database_schema_version (database, &version) == 0);
    assert (version == 4);
    assert (database_insert (database, &task) == 0);
    /* Absent checkpoint is a successful zero-version read, not an error. */
    assert (database_load_checkpoint (database, task.uuid, &actual) == 0);
    assert (actual.version == 0);
    assert (actual.durableBytes == 0);
    assert (database_save_checkpoint (database, task.uuid, &expected) == 0);
    assert (database_load_checkpoint (database, task.uuid, &actual) == 0);
    assert (actual.version == DOWNLOAD_TASK_CHECKPOINT_VERSION);
    assert (actual.durableBytes == 4096);
    assert (strcmp (actual.effectiveUrl, expected.effectiveUrl) == 0);
    assert (strcmp (actual.etag, expected.etag) == 0);
    assert (strcmp (actual.lastModified, expected.lastModified) == 0);
    assert (strcmp (actual.requestContext, expected.requestContext) == 0);
    /* Validators visible to old readers are written in the same transaction. */
    memset (&task, 0, sizeof (task));
    assert (database_load (database, "task-checkpoint", &task) == 0);
    assert (strcmp (task.etag, expected.etag) == 0);
    assert (strcmp (task.lastModified, expected.lastModified) == 0);
    expected.durableBytes = 8192;
    assert (database_save_checkpoint (database, "task-checkpoint", &expected) == 0);
    assert (database_load_checkpoint (database, "task-checkpoint", &actual) == 0);
    assert (actual.durableBytes == 8192);
    assert (database_delete_checkpoint (database, "task-checkpoint") == 0);
    assert (database_load_checkpoint (database, "task-checkpoint", &actual) == 0);
    assert (actual.version == 0);
    assert (database_save_checkpoint (database, "task-checkpoint", &expected) == 0);
    assert (database_delete (database, "task-checkpoint") == 0);
    assert (database_load_checkpoint (database, "task-checkpoint", &actual) == 0);
    assert (actual.version == 0);
    database_close (database);
}

static void test_legacy_migration (void)
{
    char                  path[] = "/tmp/downloader-database-legacy-XXXXXX";
    int                   descriptor = mkstemp (path);
    sqlite3*              raw;
    Database*             database;
    DownloadTask          task;
    DownloadTaskCheckpoint checkpoint;
    int                   version = -1;

    assert (descriptor >= 0);
    assert (close (descriptor) == 0);
    assert (sqlite3_open (path, &raw) == SQLITE_OK);
    assert (sqlite3_exec (raw, gcLegacySchema, NULL, NULL, NULL) == SQLITE_OK);
    assert (sqlite3_exec (raw, gcLegacyRow, NULL, NULL, NULL) == SQLITE_OK);
    assert (sqlite3_exec (raw, "PRAGMA user_version = 0;", NULL, NULL, NULL) == SQLITE_OK);
    assert (sqlite3_close (raw) == SQLITE_OK);
    assert (database_open (path, &database) == 0);
    assert (database_schema_version (database, &version) == 0);
    assert (version == 4);
    assert (database_load (database, "legacy-1", &task) == 0);
    assert (strcmp (task.fileName, "legacy.bin") == 0);
    assert (task.downloadedBytes == 512);
    assert (strcmp (task.etag, "\"legacy-etag\"") == 0);
    assert (database_load_checkpoint (database, "legacy-1", &checkpoint) == 0);
    assert (checkpoint.version == 0);
    assert (database_save_checkpoint (database, "legacy-1", &(DownloadTaskCheckpoint){.version = 1,
        .durableBytes = 512}) == 0);
    assert (database_load_checkpoint (database, "legacy-1", &checkpoint) == 0);
    assert (checkpoint.durableBytes == 512);
    database_close (database);
    assert (unlink (path) == 0);
}

static void test_migration_failure_rolls_back (void)
{
    char     path[] = "/tmp/downloader-database-rollback-XXXXXX";
    int      descriptor = mkstemp (path);
    sqlite3* raw;
    Database* database = NULL;
    int      count = 0;
    int      version = -1;

    assert (descriptor >= 0);
    assert (close (descriptor) == 0);
    assert (sqlite3_open (path, &raw) == SQLITE_OK);
    assert (sqlite3_exec (raw, gcLegacySchema, NULL, NULL, NULL) == SQLITE_OK);
    assert (sqlite3_exec (raw, gcLegacyRow, NULL, NULL, NULL) == SQLITE_OK);
    /* A conflicting object makes the checkpoint DDL fail inside the migration. */
    assert (sqlite3_exec (raw, "CREATE VIEW task_checkpoints AS SELECT 1 AS conflict;", NULL, NULL, NULL) ==
            SQLITE_OK);
    assert (sqlite3_close (raw) == SQLITE_OK);
    errno = 0;
    assert (database_open (path, &database) != 0);
    assert (database == NULL);
    assert (sqlite3_open (path, &raw) == SQLITE_OK);
    sqlite3_stmt* statement;
    assert (sqlite3_prepare_v2 (raw, "PRAGMA user_version;", -1, &statement, NULL) == SQLITE_OK);
    assert (sqlite3_step (statement) == SQLITE_ROW);
    version = sqlite3_column_int (statement, 0);
    sqlite3_finalize (statement);
    assert (version == 0);
    assert (sqlite3_prepare_v2 (raw, "SELECT COUNT(*) FROM download_tasks;", -1, &statement, NULL) == SQLITE_OK);
    assert (sqlite3_step (statement) == SQLITE_ROW);
    count = sqlite3_column_int (statement, 0);
    sqlite3_finalize (statement);
    assert (count == 1);
    assert (sqlite3_close (raw) == SQLITE_OK);
    assert (unlink (path) == 0);
}

static void test_newer_schema_is_rejected (void)
{
    char      path[] = "/tmp/downloader-database-newer-XXXXXX";
    int       descriptor = mkstemp (path);
    sqlite3*  raw;
    Database* database = NULL;

    assert (descriptor >= 0);
    assert (close (descriptor) == 0);
    assert (sqlite3_open (path, &raw) == SQLITE_OK);
    assert (sqlite3_exec (raw, gcLegacySchema, NULL, NULL, NULL) == SQLITE_OK);
    assert (sqlite3_exec (raw, "PRAGMA user_version = 99;", NULL, NULL, NULL) == SQLITE_OK);
    assert (sqlite3_close (raw) == SQLITE_OK);
    errno = 0;
    assert (database_open (path, &database) != 0);
    assert (database == NULL);
    assert (errno == ENOTSUP);
    assert (unlink (path) == 0);
}

int main (void)
{
    char  path[] = "/tmp/downloader-database-XXXXXX";
    int   descriptor = mkstemp (path);

    assert (descriptor >= 0);
    assert (close (descriptor) == 0);
    assert (unlink (path) == 0);
    test_insert_and_load (path);
    test_recovery_and_clear (path);
    test_group_fields (path);
    test_checkpoint_persistence (path);
    assert (unlink (path) == 0);
    test_legacy_migration ();
    test_migration_failure_rolls_back ();
    test_newer_schema_is_rejected ();
    return 0;
}
