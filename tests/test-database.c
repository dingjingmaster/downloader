#define _GNU_SOURCE

#include "database.h"
#include "task.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    assert (database_update (database, &actual) == 0);
    memset (&actual, 0, sizeof (actual));
    assert (database_load (database, expected.uuid, &actual) == 0);
    assert (actual.state == DOWNLOAD_TASK_STATE_DOWNLOADING);
    assert (actual.downloadedBytes == 500);
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

int main (void)
{
    char  path[] = "/tmp/downloader-database-XXXXXX";
    int   descriptor = mkstemp (path);

    assert (descriptor >= 0);
    assert (close (descriptor) == 0);
    assert (unlink (path) == 0);
    test_insert_and_load (path);
    test_recovery_and_clear (path);
    assert (unlink (path) == 0);
    return 0;
}
