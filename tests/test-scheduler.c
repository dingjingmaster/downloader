#define _GNU_SOURCE

#include "filesystem.h"
#include "scheduler.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_directory (const char* path)
{
    assert (mkdir (path, 0700) == 0);
}

static void test_filesystem (const char* root)
{
    DownloadTask task;
    char         targetDirectory[512];
    char         targetPath[512];
    FILE*        file;
    char         content[32] = { 0 };

    snprintf (targetDirectory, sizeof (targetDirectory), "%s/downloads", root);
    snprintf (targetPath, sizeof (targetPath), "%s/sample.bin", targetDirectory);
    make_directory (targetDirectory);
    memset (&task, 0, sizeof (task));
    snprintf (task.fileName, sizeof (task.fileName), "sample.bin");
    snprintf (task.targetDirectory, sizeof (task.targetDirectory), "%s", targetDirectory);
    snprintf (task.targetPath, sizeof (task.targetPath), "%s", targetPath);
    assert (filesystem_prepare (&task) == 0);
    file = fopen (task.tempPath, "wb");
    assert (file != NULL);
    assert (fputs ("payload", file) >= 0);
    assert (fclose (file) == 0);
    assert (filesystem_commit (&task) == 0);
    file = fopen (task.targetPath, "rb");
    assert (file != NULL);
    assert (fread (content, 1, sizeof (content) - 1, file) == 7);
    assert (fclose (file) == 0);
    assert (strcmp (content, "payload") == 0);

    snprintf (task.targetPath, sizeof (task.targetPath), "%s/../unsafe", targetDirectory);
    errno = 0;
    assert (filesystem_prepare (&task) != 0);
    assert (errno == EINVAL);
}

static volatile int gExecuted;

static int run_task (DownloadTask* task, void* userData)
{
    (void)task;
    (void)userData;
    gExecuted++;
    return 0;
}

static void test_scheduler (void)
{
    Scheduler*    scheduler;
    DownloadTask  task;
    unsigned int  index;

    memset (&task, 0, sizeof (task));
    scheduler = scheduler_new (2, run_task, NULL);
    assert (scheduler != NULL);
    assert (scheduler_start (scheduler) == 0);
    for (index = 0; index < 3; index++)
        assert (scheduler_submit (scheduler, &task) == 0);
    for (index = 0; index < 100 && gExecuted < 3; index++)
        usleep (1000);
    assert (gExecuted == 3);
    scheduler_stop (scheduler);
}

int main (void)
{
    char  templatePath[] = "/tmp/downloader-scheduler-XXXXXX";
    char* root = mkdtemp (templatePath);

    assert (root != NULL);
    test_filesystem (root);
    test_scheduler ();
    return 0;
}
