#ifndef DOWNLOADER_DATABASE_H
#define DOWNLOADER_DATABASE_H

#include "task.h"

#include <stddef.h>

typedef struct Database Database;

int  database_open (const char* path, Database** result);
void database_close (Database* database);
int  database_insert (Database* database, const DownloadTask* task);
/* Inserts all tasks in one transaction; a failure rolls back the whole batch. */
int  database_insert_many (Database* database, const DownloadTask* tasks, size_t count);
int  database_update (Database* database, const DownloadTask* task);
int  database_load (Database* database, const char* uuid, DownloadTask* task);
int  database_list (Database* database, size_t offset, size_t limit, DownloadTask** tasks, size_t* count, size_t* total);
/* Returns the group's tasks in creation order; free *tasks with c_free(). */
int  database_list_group (Database* database, const char* groupUuid, DownloadTask** tasks, size_t* count);
/* Aggregates one group's task count, terminal states and byte totals. */
int  database_group_summary (Database* database, const char* groupUuid, DownloadTaskGroupSummary* summary);
int  database_delete (Database* database, const char* uuid);
int  database_recover (Database* database);
int  database_clear_completed (Database* database);
/* Current schema version, advanced only by transactionally verified migrations. */
int  database_schema_version (Database* database, int* version);
/* Checkpoint text is persisted together with the task validators in one transaction. */
int  database_save_checkpoint (Database* database, const char* uuid, const DownloadTaskCheckpoint* checkpoint);
/* Returns 0 with a zeroed version when no checkpoint exists, so legacy tasks degrade safely. */
int  database_load_checkpoint (Database* database, const char* uuid, DownloadTaskCheckpoint* checkpoint);
int  database_delete_checkpoint (Database* database, const char* uuid);

#endif
