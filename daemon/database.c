#include "database.h"

#include <clib/memory.h>

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Database
{
    sqlite3* handle;
};

static const char gcDatabaseSchema[] =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS download_tasks ("
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
    ");"
    "CREATE INDEX IF NOT EXISTS idx_download_tasks_state ON download_tasks(state);";

static int database_set_errno (Database* database)
{
    int errorCode = sqlite3_errcode (database->handle);

    errno = errorCode == SQLITE_NOMEM ? ENOMEM : EIO;
    return -1;
}

static int database_exec (Database* database, const char* sql)
{
    char* errorMessage = NULL;
    int   result;

    result = sqlite3_exec (database->handle, sql, NULL, NULL, &errorMessage);
    sqlite3_free (errorMessage);
    return result == SQLITE_OK ? 0 : database_set_errno (database);
}

static void database_copy_text (char* destination, size_t destinationSize, const unsigned char* source)
{
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf ((char*)destination, destinationSize, "%s", source);
}

static void database_read_task (sqlite3_stmt* statement, DownloadTask* task)
{
    memset (task, 0, sizeof (*task));
    database_copy_text (task->uuid, sizeof (task->uuid), sqlite3_column_text (statement, 0));
    database_copy_text (task->fileName, sizeof (task->fileName), sqlite3_column_text (statement, 1));
    database_copy_text (task->targetDirectory, sizeof (task->targetDirectory), sqlite3_column_text (statement, 2));
    database_copy_text (task->targetPath, sizeof (task->targetPath), sqlite3_column_text (statement, 3));
    database_copy_text (task->tempPath, sizeof (task->tempPath), sqlite3_column_text (statement, 4));
    database_copy_text (task->resourceUrl, sizeof (task->resourceUrl), sqlite3_column_text (statement, 5));
    database_copy_text (task->protocol, sizeof (task->protocol), sqlite3_column_text (statement, 6));
    task->state = download_task_state_from_name ((const char*)sqlite3_column_text (statement, 7));
    task->errorCode = sqlite3_column_int (statement, 8);
    database_copy_text (task->errorMessage, sizeof (task->errorMessage), sqlite3_column_text (statement, 9));
    task->totalBytes = (uint64_t)sqlite3_column_int64 (statement, 10);
    task->downloadedBytes = (uint64_t)sqlite3_column_int64 (statement, 11);
    task->progress = sqlite3_column_double (statement, 12);
    task->speedBytesSec = (uint64_t)sqlite3_column_int64 (statement, 13);
    task->createdAt = sqlite3_column_int64 (statement, 14);
    task->startedAt = sqlite3_column_int64 (statement, 15);
    task->pausedAt = sqlite3_column_int64 (statement, 16);
    task->completedAt = sqlite3_column_int64 (statement, 17);
    task->updatedAt = sqlite3_column_int64 (statement, 18);
    task->retryCount = sqlite3_column_int (statement, 19);
    task->maxRetries = sqlite3_column_int (statement, 20);
    database_copy_text (task->checksumType, sizeof (task->checksumType), sqlite3_column_text (statement, 21));
    database_copy_text (task->checksumValue, sizeof (task->checksumValue), sqlite3_column_text (statement, 22));
    database_copy_text (task->etag, sizeof (task->etag), sqlite3_column_text (statement, 23));
    database_copy_text (task->lastModified, sizeof (task->lastModified), sqlite3_column_text (statement, 24));
    database_copy_text (task->extraJson, sizeof (task->extraJson), sqlite3_column_text (statement, 25));
}

static int database_bind_task (sqlite3_stmt* statement, const DownloadTask* task)
{
    int index = 1;

    if (sqlite3_bind_text (statement, index++, task->uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->fileName, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->targetDirectory, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->targetPath, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->tempPath, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->resourceUrl, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->protocol, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, download_task_state_name (task->state), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_int (statement, index++, task->errorCode) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->errorMessage, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->totalBytes) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->downloadedBytes) != SQLITE_OK ||
        sqlite3_bind_double (statement, index++, task->progress) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->speedBytesSec) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->createdAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->startedAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->pausedAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->completedAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->updatedAt) != SQLITE_OK ||
        sqlite3_bind_int (statement, index++, task->retryCount) != SQLITE_OK ||
        sqlite3_bind_int (statement, index++, task->maxRetries) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->checksumType, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->checksumValue, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->etag, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->lastModified, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->extraJson, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int database_open (const char* path, Database** result)
{
    Database* database;

    if (path == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }
    database = (Database*)c_malloc0 (sizeof (*database));
    if (database == NULL)
        return -1;
    if (sqlite3_open_v2 (path, &database->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        sqlite3_close (database->handle);
        c_free (database);
        errno = EIO;
        return -1;
    }
    if (database_exec (database, gcDatabaseSchema) != 0) {
        database_close (database);
        return -1;
    }
    *result = database;
    return 0;
}

void database_close (Database* database)
{
    if (database == NULL)
        return;
    sqlite3_close (database->handle);
    c_free (database);
}

int database_insert (Database* database, const DownloadTask* task)
{
    static const char sql[] =
        "INSERT INTO download_tasks (uuid,file_name,target_directory,target_path,temp_path,resource_url,protocol,state,"
        "error_code,error_message,total_bytes,downloaded_bytes,progress,speed_bytes_sec,created_at,started_at,paused_at,"
        "completed_at,updated_at,retry_count,max_retries,checksum_type,checksum_value,etag,last_modified,extra_json) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    result = database_bind_task (statement, task);
    if (result == 0 && sqlite3_step (statement) != SQLITE_DONE)
        result = database_set_errno (database);
    sqlite3_finalize (statement);
    return result;
}

int database_update (Database* database, const DownloadTask* task)
{
    static const char sql[] =
        "UPDATE download_tasks SET file_name=?,target_directory=?,target_path=?,temp_path=?,resource_url=?,protocol=?,"
        "state=?,error_code=?,error_message=?,total_bytes=?,downloaded_bytes=?,progress=?,speed_bytes_sec=?,created_at=?,"
        "started_at=?,paused_at=?,completed_at=?,updated_at=?,retry_count=?,max_retries=?,checksum_type=?,checksum_value=?,"
        "etag=?,last_modified=?,extra_json=? WHERE uuid=?;";
    sqlite3_stmt* statement;
    int           index = 1;
    int           result;

    if (database == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    if (sqlite3_bind_text (statement, index++, task->fileName, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->targetDirectory, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->targetPath, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->tempPath, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->resourceUrl, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->protocol, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, download_task_state_name (task->state), -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_int (statement, index++, task->errorCode) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->errorMessage, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->totalBytes) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->downloadedBytes) != SQLITE_OK ||
        sqlite3_bind_double (statement, index++, task->progress) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->speedBytesSec) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->createdAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->startedAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->pausedAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->completedAt) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->updatedAt) != SQLITE_OK ||
        sqlite3_bind_int (statement, index++, task->retryCount) != SQLITE_OK ||
        sqlite3_bind_int (statement, index++, task->maxRetries) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->checksumType, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->checksumValue, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->etag, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->lastModified, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->extraJson, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index, task->uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    if (result != SQLITE_DONE) {
        sqlite3_finalize (statement);
        return database_set_errno (database);
    }
    result = sqlite3_changes (database->handle);
    sqlite3_finalize (statement);
    if (result == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int database_load (Database* database, const char* uuid, DownloadTask* task)
{
    static const char sql[] = "SELECT uuid,file_name,target_directory,target_path,temp_path,resource_url,protocol,state,"
                               "error_code,error_message,total_bytes,downloaded_bytes,progress,speed_bytes_sec,created_at,"
                               "started_at,paused_at,completed_at,updated_at,retry_count,max_retries,checksum_type,"
                               "checksum_value,etag,last_modified,extra_json FROM download_tasks WHERE uuid=?;";
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || uuid == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    (void)sqlite3_bind_text (statement, 1, uuid, -1, SQLITE_TRANSIENT);
    result = sqlite3_step (statement);
    if (result == SQLITE_ROW) {
        database_read_task (statement, task);
        result = 0;
    } else if (result == SQLITE_DONE) {
        errno = ENOENT;
        result = -1;
    } else {
        result = database_set_errno (database);
    }
    sqlite3_finalize (statement);
    return result;
}

int database_list (Database* database, size_t offset, size_t limit, DownloadTask** tasks, size_t* count, size_t* total)
{
    static const char sql[] = "SELECT uuid,file_name,target_directory,target_path,temp_path,resource_url,protocol,state,"
                               "error_code,error_message,total_bytes,downloaded_bytes,progress,speed_bytes_sec,created_at,"
                               "started_at,paused_at,completed_at,updated_at,retry_count,max_retries,checksum_type,"
                               "checksum_value,etag,last_modified,extra_json FROM download_tasks ORDER BY id DESC LIMIT ? OFFSET ?;";
    static const char countSql[] = "SELECT COUNT(*) FROM download_tasks;";
    sqlite3_stmt* statement;
    sqlite3_stmt* countStatement;
    DownloadTask* values;
    size_t        index = 0;
    int           result;

    if (database == NULL || tasks == NULL || count == NULL || total == NULL || limit == 0 ||
        limit > SIZE_MAX / sizeof (DownloadTask)) {
        errno = EINVAL;
        return -1;
    }
    *tasks = NULL;
    *count = 0;
    *total = 0;
    if (sqlite3_prepare_v2 (database->handle, countSql, -1, &countStatement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    result = sqlite3_step (countStatement);
    if (result != SQLITE_ROW) {
        sqlite3_finalize (countStatement);
        return database_set_errno (database);
    }
    *total = (size_t)sqlite3_column_int64 (countStatement, 0);
    sqlite3_finalize (countStatement);
    values = (DownloadTask*)c_malloc0 (limit * sizeof (*values));
    if (values == NULL)
        return -1;
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK) {
        c_free (values);
        return database_set_errno (database);
    }
    if (sqlite3_bind_int64 (statement, 1, (sqlite3_int64)limit) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, 2, (sqlite3_int64)offset) != SQLITE_OK) {
        sqlite3_finalize (statement);
        c_free (values);
        errno = ENOMEM;
        return -1;
    }
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        database_read_task (statement, &values[index++]);
    }
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE) {
        c_free (values);
        return database_set_errno (database);
    }
    *tasks = values;
    *count = index;
    return 0;
}

int database_delete (Database* database, const char* uuid)
{
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || uuid == NULL || uuid[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (database->handle, "DELETE FROM download_tasks WHERE uuid=?;", -1, &statement, NULL) !=
        SQLITE_OK)
        return database_set_errno (database);
    if (sqlite3_bind_text (statement, 1, uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE)
        return database_set_errno (database);
    if (sqlite3_changes (database->handle) == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int database_recover (Database* database)
{
    if (database == NULL) {
        errno = EINVAL;
        return -1;
    }
    return database_exec (database, "UPDATE download_tasks SET state='queued' WHERE state='downloading';");
}

int database_clear_completed (Database* database)
{
    int changed;

    if (database == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (database_exec (database, "DELETE FROM download_tasks WHERE state='completed';") != 0)
        return -1;
    changed = sqlite3_changes (database->handle);
    return changed;
}
