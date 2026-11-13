#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "filesystem.h"

#include <clib/directory.h>
#include <clib/memory.h>
#include <clib/path.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool filesystem_has_parent_reference (const char* path)
{
    const char* part = path;

    while (part != NULL && *part != '\0') {
        const char* separator = strchr (part, '/');
        size_t      size = separator == NULL ? strlen (part) : (size_t)(separator - part);

        if (size == 2U && strncmp (part, "..", 2U) == 0)
            return true;
        part = separator == NULL ? NULL : separator + 1;
    }
    return false;
}

static bool filesystem_path_is_inside (const char* directory, const char* path)
{
    size_t directorySize;

    directorySize = strlen (directory);
    return strncmp (directory, path, directorySize) == 0 &&
           (path[directorySize] == '\0' || path[directorySize] == '/');
}

int filesystem_prepare (DownloadTask* task)
{
    char* temporaryPath;

    if (task == NULL || task->targetDirectory[0] == '\0' || task->fileName[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (filesystem_has_parent_reference (task->targetDirectory) || filesystem_has_parent_reference (task->fileName) ||
        filesystem_has_parent_reference (task->targetPath)) {
        errno = EINVAL;
        return -1;
    }
    if (c_directory_create_parents (task->targetDirectory) != 0)
        return -1;
    if (task->targetPath[0] == '\0') {
        temporaryPath = c_path_join (task->targetDirectory, task->fileName);
        if (temporaryPath == NULL)
            return -1;
        if (snprintf (task->targetPath, sizeof (task->targetPath), "%s", temporaryPath) >=
            (int)sizeof (task->targetPath)) {
            c_free (temporaryPath);
            errno = ENAMETOOLONG;
            return -1;
        }
        c_free (temporaryPath);
    }
    if (!filesystem_path_is_inside (task->targetDirectory, task->targetPath)) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf (task->tempPath, sizeof (task->tempPath), "%s.part", task->targetPath) >=
        (int)sizeof (task->tempPath)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int filesystem_commit (const DownloadTask* task)
{
    int descriptor;

    if (task == NULL || task->tempPath[0] == '\0' || task->targetPath[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (filesystem_has_parent_reference (task->tempPath) || filesystem_has_parent_reference (task->targetPath)) {
        errno = EINVAL;
        return -1;
    }
    if (lstat (task->targetPath, &(struct stat){ 0 }) == 0) {
        errno = EEXIST;
        return -1;
    }
    descriptor = open (task->tempPath, O_WRONLY | O_APPEND | O_CLOEXEC);
    if (descriptor < 0)
        return -1;
    if (fsync (descriptor) != 0) {
        int savedErrno = errno;
        (void)close (descriptor);
        errno = savedErrno;
        return -1;
    }
    if (close (descriptor) != 0)
        return -1;
    if (rename (task->tempPath, task->targetPath) != 0)
        return -1;
    return 0;
}
