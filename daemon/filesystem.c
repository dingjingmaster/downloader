#define _GNU_SOURCE

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
    if (directorySize == 1 && directory[0] == '/')
        return path[0] == '/' && path[1] != '\0';
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
    temporaryPath = c_path_canonicalize (task->targetDirectory, NULL);
    if (temporaryPath == NULL)
        return -1;
    int directoryLength = snprintf (task->targetDirectory, sizeof (task->targetDirectory), "%s", temporaryPath);
    c_free (temporaryPath);
    if (directoryLength >= (int)sizeof (task->targetDirectory)) {
        errno = ENAMETOOLONG;
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
    int written = task->uuid[0] ?
        snprintf (task->tempPath, sizeof (task->tempPath), "%s/.%s.part", task->targetDirectory, task->uuid) :
        snprintf (task->tempPath, sizeof (task->tempPath), "%s.part", task->targetPath);
    if (written >=
        (int)sizeof (task->tempPath)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int filesystem_sync_tree (const char* path)
{
    struct stat status;
    int descriptor;
    int result = 0;
    if (lstat (path, &status) != 0)
        return -1;
    if (!S_ISREG (status.st_mode) && !S_ISDIR (status.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    descriptor = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return -1;
    if (S_ISDIR (status.st_mode)) {
        CDirectory* directory = c_directory_open (path);
        const char* name;
        int next = 0;
        if (directory == NULL) {
            result = -1;
        } else {
            while (result == 0 && (next = c_directory_next (directory, &name)) > 0) {
                char* child = c_path_join (path, name);
                if (child == NULL || filesystem_sync_tree (child) != 0)
                    result = -1;
                int savedErrno = errno;
                c_free (child);
                errno = savedErrno;
            }
            if (next < 0)
                result = -1;
            int savedErrno = errno;
            c_directory_free (directory);
            errno = savedErrno;
        }
    }
    if (result == 0)
        result = fsync (descriptor);
    int savedErrno = errno;
    if (close (descriptor) != 0 && result == 0)
        return -1;
    errno = savedErrno;
    return result;
}

int filesystem_remove_partial (const DownloadTask* task)
{
    struct stat status;
    if (task == NULL || task->tempPath[0] == '\0' || filesystem_has_parent_reference (task->tempPath)) {
        errno = EINVAL;
        return -1;
    }
    if (lstat (task->tempPath, &status) != 0)
        return errno == ENOENT ? 0 : -1;
    return S_ISDIR (status.st_mode) ? c_directory_remove_recursive (task->tempPath) : unlink (task->tempPath);
}

int filesystem_commit (const DownloadTask* task)
{

    if (task == NULL || task->tempPath[0] == '\0' || task->targetPath[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (filesystem_has_parent_reference (task->tempPath) || filesystem_has_parent_reference (task->targetPath)) {
        errno = EINVAL;
        return -1;
    }
    if (filesystem_sync_tree (task->tempPath) != 0)
        return -1;
    /* Atomic no-replace also prevents concurrent tasks from overwriting results. */
    if (renameat2 (AT_FDCWD, task->tempPath, AT_FDCWD, task->targetPath, RENAME_NOREPLACE) != 0)
        return -1;
    int descriptor = open (task->targetDirectory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        return 1;
    int result = fsync (descriptor);
    int savedErrno = errno;
    if (close (descriptor) != 0 && result == 0)
        return 1;
    errno = savedErrno;
    return result == 0 ? 0 : 1;
}
