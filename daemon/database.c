#include "database.h"

#include <clib/memory.h>

#include <errno.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOWNLOADER_DATABASE_VERSION 4

struct Database
{
    sqlite3* handle;
};

static const char gcDatabaseSchema[] =
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
    "extra_json TEXT NOT NULL DEFAULT '{}',"
    "group_uuid TEXT NOT NULL DEFAULT '',"
    "group_name TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_download_tasks_state ON download_tasks(state);";

static const char gcCredentialSchema[] =
    "CREATE TABLE IF NOT EXISTS credential_profiles ("
    "name TEXT PRIMARY KEY,"
    "kind TEXT NOT NULL,"
    "username TEXT NOT NULL DEFAULT '',"
    "secret BLOB NOT NULL,"
    "scope TEXT NOT NULL DEFAULT '',"
    "created_at INTEGER NOT NULL DEFAULT 0,"
    "updated_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS proxy_profiles ("
    "name TEXT PRIMARY KEY,"
    "url TEXT NOT NULL,"
    "username TEXT NOT NULL DEFAULT '',"
    "secret BLOB NOT NULL,"
    "created_at INTEGER NOT NULL DEFAULT 0,"
    "updated_at INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS site_profiles ("
    "domain TEXT PRIMARY KEY,"
    "auth_profile TEXT NOT NULL DEFAULT '',"
    "proxy_profile TEXT NOT NULL DEFAULT '',"
    "updated_at INTEGER NOT NULL DEFAULT 0"
    ");";

static const char gcCheckpointSchema[] =
    "CREATE TABLE IF NOT EXISTS task_checkpoints ("
    "task_uuid TEXT PRIMARY KEY REFERENCES download_tasks(uuid) ON DELETE CASCADE,"
    "version INTEGER NOT NULL DEFAULT 0,"
    "effective_url TEXT NOT NULL DEFAULT '',"
    "etag TEXT NOT NULL DEFAULT '',"
    "last_modified TEXT NOT NULL DEFAULT '',"
    "durable_bytes INTEGER NOT NULL DEFAULT 0,"
    "request_context TEXT NOT NULL DEFAULT '',"
    "updated_at INTEGER NOT NULL DEFAULT 0"
    ");";

static const char gcInsertTaskSql[] =
    "INSERT INTO download_tasks (uuid,file_name,target_directory,target_path,temp_path,resource_url,protocol,state,"
    "error_code,error_message,total_bytes,downloaded_bytes,progress,speed_bytes_sec,created_at,started_at,paused_at,"
    "completed_at,updated_at,retry_count,max_retries,checksum_type,checksum_value,etag,last_modified,extra_json,"
    "duration_millis,gap_count,group_uuid,group_name) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

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

static int database_read_version (Database* database, int* version)
{
    sqlite3_stmt* statement;
    int           result;

    if (sqlite3_prepare_v2 (database->handle, "PRAGMA user_version;", -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    result = sqlite3_step (statement);
    if (result == SQLITE_ROW) {
        *version = sqlite3_column_int (statement, 0);
        result = 0;
    } else {
        result = database_set_errno (database);
    }
    sqlite3_finalize (statement);
    return result;
}

static int database_column_exists (Database* database, const char* table, const char* column, bool* exists)
{
    sqlite3_stmt* statement;
    int           result;

    *exists = false;
    if (sqlite3_prepare_v2 (database->handle, "PRAGMA table_info(download_tasks);", -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text (statement, 1);
        if (name != NULL && strcmp (name, column) == 0) {
            *exists = true;
            break;
        }
    }
    sqlite3_finalize (statement);
    (void)table;
    return result == SQLITE_ROW || result == SQLITE_DONE ? 0 : database_set_errno (database);
}

/* A view or index with the expected name satisfies CREATE ... IF NOT EXISTS
 * but cannot store our rows, so the migration must reject it. */
static int database_verify_table (Database* database, const char* name, bool* usable)
{
    sqlite3_stmt* statement;
    int           result;

    *usable = true;
    if (sqlite3_prepare_v2 (database->handle, "SELECT type FROM sqlite_master WHERE name=?;", -1, &statement,
                            NULL) != SQLITE_OK)
        return database_set_errno (database);
    if (sqlite3_bind_text (statement, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    if (result == SQLITE_ROW)
        *usable = strcmp ((const char*)sqlite3_column_text (statement, 0), "table") == 0;
    else if (result != SQLITE_DONE)
        result = database_set_errno (database);
    if (result == SQLITE_ROW || result == SQLITE_DONE)
        result = 0;
    sqlite3_finalize (statement);
    return result;
}

static int database_verify_tables (Database* database, const char* const* names, size_t count)
{
    for (size_t index = 0; index < count; index++) {
        bool usable = true;
        if (database_verify_table (database, names[index], &usable) != 0)
            return -1;
        if (!usable) {
            errno = EIO;
            return -1;
        }
    }
    return 0;
}

/* Runs every schema change in one transaction: a failing migration leaves the
 * user's database exactly as it was, with its previous user_version. */
static int database_migrate (Database* database)
{
    static const char* const checkpointTables[] = { "task_checkpoints" };
    static const char* const credentialTables[] = { "credential_profiles", "proxy_profiles", "site_profiles" };
    int  version;
    char sql[64];

    if (database_read_version (database, &version) != 0)
        return -1;
    if (version > DOWNLOADER_DATABASE_VERSION) {
        errno = ENOTSUP;
        return -1;
    }
    if (database_exec (database, "BEGIN IMMEDIATE;") != 0)
        return -1;
    if (version < 1 &&
        database_verify_tables (database, checkpointTables, sizeof (checkpointTables) / sizeof (checkpointTables[0])) != 0)
        goto failed;
    if (version < 2 &&
        database_verify_tables (database, credentialTables, sizeof (credentialTables) / sizeof (credentialTables[0])) != 0)
        goto failed;
    if (database_exec (database, gcDatabaseSchema) != 0 ||
        (version < 1 && database_exec (database, gcCheckpointSchema) != 0) ||
        (version < 2 && database_exec (database, gcCredentialSchema) != 0))
        goto failed;
    if (version < 3) {
        static const char* const columns[] = { "duration_millis", "gap_count" };
        for (size_t index = 0; index < sizeof (columns) / sizeof (columns[0]); index++) {
            bool exists = false;
            char sql[128];
            if (database_column_exists (database, "download_tasks", columns[index], &exists) != 0)
                goto failed;
            if (exists)
                continue;
            snprintf (sql, sizeof (sql),
                      "ALTER TABLE download_tasks ADD COLUMN %s INTEGER NOT NULL DEFAULT 0;", columns[index]);
            if (database_exec (database, sql) != 0)
                goto failed;
        }
    }
    if (version < 4) {
        static const char* const columns[] = { "group_uuid", "group_name" };
        for (size_t index = 0; index < sizeof (columns) / sizeof (columns[0]); index++) {
            bool exists = false;
            char columnSql[128];
            if (database_column_exists (database, "download_tasks", columns[index], &exists) != 0)
                goto failed;
            if (exists)
                continue;
            snprintf (columnSql, sizeof (columnSql),
                      "ALTER TABLE download_tasks ADD COLUMN %s TEXT NOT NULL DEFAULT '';", columns[index]);
            if (database_exec (database, columnSql) != 0)
                goto failed;
        }
        if (database_exec (database,
                           "CREATE INDEX IF NOT EXISTS idx_download_tasks_group ON download_tasks(group_uuid);") != 0)
            goto failed;
    }
    if (version < DOWNLOADER_DATABASE_VERSION) {
        snprintf (sql, sizeof (sql), "PRAGMA user_version = %d;", DOWNLOADER_DATABASE_VERSION);
        if (database_exec (database, sql) != 0)
            goto failed;
    }
    if (database_exec (database, "COMMIT;") == 0)
        return 0;
failed:
    {
        int savedErrno = errno;
        (void)database_exec (database, "ROLLBACK;");
        errno = savedErrno;
        return -1;
    }
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
    task->durationMillis = sqlite3_column_int64 (statement, 26);
    task->gapCount = (uint32_t)sqlite3_column_int64 (statement, 27);
    database_copy_text (task->groupUuid, sizeof (task->groupUuid), sqlite3_column_text (statement, 28));
    database_copy_text (task->groupName, sizeof (task->groupName), sqlite3_column_text (statement, 29));
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
        sqlite3_bind_text (statement, index++, task->extraJson, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, task->durationMillis) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->gapCount) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->groupUuid, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->groupName, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
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
    /* External readers (the Web API and migration tooling) may hold short
     * locks; transient contention must not abort a running download. */
    sqlite3_busy_timeout (database->handle, 5000);
    /* The pragma is connection-scoped and must run outside the migration transaction. */
    if (database_exec (database, "PRAGMA foreign_keys = ON;") != 0 || database_migrate (database) != 0) {
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
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (database->handle, gcInsertTaskSql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    result = database_bind_task (statement, task);
    if (result == 0 && sqlite3_step (statement) != SQLITE_DONE)
        result = database_set_errno (database);
    sqlite3_finalize (statement);
    return result;
}

int database_insert_many (Database* database, const DownloadTask* tasks, size_t count)
{
    sqlite3_stmt* statement;
    size_t        index;
    int           result = 0;

    if (database == NULL || (tasks == NULL && count > 0)) {
        errno = EINVAL;
        return -1;
    }
    if (count == 0)
        return 0;
    if (sqlite3_prepare_v2 (database->handle, gcInsertTaskSql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    if (database_exec (database, "BEGIN IMMEDIATE;") != 0) {
        sqlite3_finalize (statement);
        return -1;
    }
    for (index = 0; index < count; index++) {
        (void)sqlite3_reset (statement);
        (void)sqlite3_clear_bindings (statement);
        result = database_bind_task (statement, &tasks[index]);
        if (result == 0 && sqlite3_step (statement) != SQLITE_DONE)
            result = database_set_errno (database);
        if (result != 0)
            break;
    }
    sqlite3_finalize (statement);
    if (result == 0 && database_exec (database, "COMMIT;") == 0)
        return 0;
    {
        int savedErrno = errno;
        (void)database_exec (database, "ROLLBACK;");
        errno = savedErrno;
    }
    return -1;
}

int database_update (Database* database, const DownloadTask* task)
{
    static const char sql[] =
        "UPDATE download_tasks SET file_name=?,target_directory=?,target_path=?,temp_path=?,resource_url=?,protocol=?,"
        "state=?,error_code=?,error_message=?,total_bytes=?,downloaded_bytes=?,progress=?,speed_bytes_sec=?,created_at=?,"
        "started_at=?,paused_at=?,completed_at=?,updated_at=?,retry_count=?,max_retries=?,checksum_type=?,checksum_value=?,"
        "etag=?,last_modified=?,extra_json=?,duration_millis=?,gap_count=?,group_uuid=?,group_name=? WHERE uuid=?;";
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
        sqlite3_bind_int64 (statement, index++, task->durationMillis) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, index++, (sqlite3_int64)task->gapCount) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->groupUuid, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, index++, task->groupName, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
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
                               "checksum_value,etag,last_modified,extra_json,duration_millis,gap_count,group_uuid,group_name FROM download_tasks WHERE uuid=?;";
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
                               "checksum_value,etag,last_modified,extra_json,duration_millis,gap_count,group_uuid,group_name FROM download_tasks ORDER BY id DESC LIMIT ? OFFSET ?;";
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

int database_list_group (Database* database, const char* groupUuid, DownloadTask** tasks, size_t* count)
{
    static const char sql[] = "SELECT uuid,file_name,target_directory,target_path,temp_path,resource_url,protocol,state,"
                               "error_code,error_message,total_bytes,downloaded_bytes,progress,speed_bytes_sec,created_at,"
                               "started_at,paused_at,completed_at,updated_at,retry_count,max_retries,checksum_type,"
                               "checksum_value,etag,last_modified,extra_json,duration_millis,gap_count,group_uuid,group_name "
                               "FROM download_tasks WHERE group_uuid=? ORDER BY id ASC;";
    sqlite3_stmt* statement;
    DownloadTask* values = NULL;
    size_t        capacity = 0;
    size_t        used = 0;
    int           result;

    if (database == NULL || groupUuid == NULL || tasks == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *tasks = NULL;
    *count = 0;
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    if (sqlite3_bind_text (statement, 1, groupUuid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
        if (used == capacity) {
            size_t        next = capacity == 0 ? 16U : capacity * 2U;
            DownloadTask* grown;
            if (next < capacity) {
                errno = EOVERFLOW;
                result = -1;
                break;
            }
            grown = (DownloadTask*)c_realloc_array (values, next, sizeof (*values));
            if (grown == NULL) {
                result = -1;
                break;
            }
            values = grown;
            capacity = next;
        }
        database_read_task (statement, &values[used++]);
    }
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE) {
        int savedErrno;
        if (result == -1) {
            savedErrno = errno;
        } else {
            (void)database_set_errno (database);
            savedErrno = errno;
        }
        c_free (values);
        errno = savedErrno;
        return -1;
    }
    *tasks = values;
    *count = used;
    return 0;
}

int database_group_summary (Database* database, const char* groupUuid, DownloadTaskGroupSummary* summary)
{
    static const char sql[] =
        "SELECT group_name, COUNT(*), COALESCE(SUM(state='completed'), 0), COALESCE(SUM(state='failed'), 0),"
        " COALESCE(SUM(total_bytes), 0), COALESCE(SUM(downloaded_bytes), 0)"
        " FROM download_tasks WHERE group_uuid=?;";
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || groupUuid == NULL || summary == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (summary, 0, sizeof (*summary));
    (void)snprintf (summary->groupUuid, sizeof (summary->groupUuid), "%s", groupUuid);
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    if (sqlite3_bind_text (statement, 1, groupUuid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    if (result != SQLITE_ROW) {
        sqlite3_finalize (statement);
        return database_set_errno (database);
    }
    database_copy_text (summary->groupName, sizeof (summary->groupName), sqlite3_column_text (statement, 0));
    summary->taskCount = (size_t)sqlite3_column_int64 (statement, 1);
    summary->completedCount = (size_t)sqlite3_column_int64 (statement, 2);
    summary->failedCount = (size_t)sqlite3_column_int64 (statement, 3);
    summary->totalBytes = (uint64_t)sqlite3_column_int64 (statement, 4);
    summary->downloadedBytes = (uint64_t)sqlite3_column_int64 (statement, 5);
    sqlite3_finalize (statement);
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

int database_schema_version (Database* database, int* version)
{
    if (database == NULL || version == NULL) {
        errno = EINVAL;
        return -1;
    }
    return database_read_version (database, version);
}

static void database_read_checkpoint (sqlite3_stmt* statement, DownloadTaskCheckpoint* checkpoint)
{
    memset (checkpoint, 0, sizeof (*checkpoint));
    checkpoint->version = (uint32_t)sqlite3_column_int (statement, 0);
    checkpoint->durableBytes = (uint64_t)sqlite3_column_int64 (statement, 1);
    database_copy_text (checkpoint->effectiveUrl, sizeof (checkpoint->effectiveUrl),
                        sqlite3_column_text (statement, 2));
    database_copy_text (checkpoint->etag, sizeof (checkpoint->etag), sqlite3_column_text (statement, 3));
    database_copy_text (checkpoint->lastModified, sizeof (checkpoint->lastModified),
                        sqlite3_column_text (statement, 4));
    database_copy_text (checkpoint->requestContext, sizeof (checkpoint->requestContext),
                        sqlite3_column_text (statement, 5));
}

static int database_rollback (Database* database, int errorCode)
{
    (void)database_exec (database, "ROLLBACK;");
    errno = errorCode;
    return -1;
}

int database_save_checkpoint (Database* database, const char* uuid, const DownloadTaskCheckpoint* checkpoint)
{
    static const char saveSql[] =
        "INSERT INTO task_checkpoints (task_uuid,version,effective_url,etag,last_modified,durable_bytes,"
        "request_context,updated_at) VALUES (?,?,?,?,?,?,?,CAST(strftime('%s','now') AS INTEGER)) "
        "ON CONFLICT(task_uuid) DO UPDATE SET version=excluded.version,effective_url=excluded.effective_url,"
        "etag=excluded.etag,last_modified=excluded.last_modified,durable_bytes=excluded.durable_bytes,"
        "request_context=excluded.request_context,updated_at=excluded.updated_at;";
    static const char validatorsSql[] =
        "UPDATE download_tasks SET etag=?,last_modified=? WHERE uuid=?;";
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || uuid == NULL || uuid[0] == '\0' || checkpoint == NULL ||
        checkpoint->version > DOWNLOAD_TASK_CHECKPOINT_VERSION ||
        checkpoint->durableBytes > (uint64_t)INT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (database_exec (database, "BEGIN IMMEDIATE;") != 0)
        return -1;
    if (sqlite3_prepare_v2 (database->handle, saveSql, -1, &statement, NULL) != SQLITE_OK)
        return database_rollback (database, EIO);
    if (sqlite3_bind_text (statement, 1, uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int (statement, 2, (int)checkpoint->version) != SQLITE_OK ||
        sqlite3_bind_text (statement, 3, checkpoint->effectiveUrl, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 4, checkpoint->etag, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 5, checkpoint->lastModified, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64 (statement, 6, (sqlite3_int64)checkpoint->durableBytes) != SQLITE_OK ||
        sqlite3_bind_text (statement, 7, checkpoint->requestContext, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        return database_rollback (database, ENOMEM);
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE)
        return database_rollback (database, EIO);
    if (sqlite3_prepare_v2 (database->handle, validatorsSql, -1, &statement, NULL) != SQLITE_OK)
        return database_rollback (database, EIO);
    if (sqlite3_bind_text (statement, 1, checkpoint->etag, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 2, checkpoint->lastModified, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text (statement, 3, uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        return database_rollback (database, ENOMEM);
    }
    result = sqlite3_step (statement);
    sqlite3_finalize (statement);
    if (result != SQLITE_DONE)
        return database_rollback (database, EIO);
    if (sqlite3_changes (database->handle) == 0)
        return database_rollback (database, ENOENT);
    if (database_exec (database, "COMMIT;") != 0) {
        (void)database_exec (database, "ROLLBACK;");
        return -1;
    }
    return 0;
}

int database_load_checkpoint (Database* database, const char* uuid, DownloadTaskCheckpoint* checkpoint)
{
    static const char sql[] = "SELECT version,durable_bytes,effective_url,etag,last_modified,request_context "
                              "FROM task_checkpoints WHERE task_uuid=?;";
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || uuid == NULL || uuid[0] == '\0' || checkpoint == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset (checkpoint, 0, sizeof (*checkpoint));
    if (sqlite3_prepare_v2 (database->handle, sql, -1, &statement, NULL) != SQLITE_OK)
        return database_set_errno (database);
    if (sqlite3_bind_text (statement, 1, uuid, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize (statement);
        errno = ENOMEM;
        return -1;
    }
    result = sqlite3_step (statement);
    if (result == SQLITE_ROW) {
        database_read_checkpoint (statement, checkpoint);
        result = 0;
    } else if (result == SQLITE_DONE) {
        /* Legacy tasks have no checkpoint row and resume through persisted columns. */
        result = 0;
    } else {
        result = database_set_errno (database);
    }
    sqlite3_finalize (statement);
    return result;
}

int database_delete_checkpoint (Database* database, const char* uuid)
{
    sqlite3_stmt* statement;
    int           result;

    if (database == NULL || uuid == NULL || uuid[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (sqlite3_prepare_v2 (database->handle, "DELETE FROM task_checkpoints WHERE task_uuid=?;", -1,
                            &statement, NULL) != SQLITE_OK)
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
    return 0;
}
