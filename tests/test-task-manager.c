#define _GNU_SOURCE

#include "database.h"
#include "task-manager.h"

#include <clib/memory.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    assert (unlink (databasePath) == 0);
    return 0;
}
