#ifndef DOWNLOADER_DATABASE_H
#define DOWNLOADER_DATABASE_H

#include "task.h"

#include <stddef.h>

typedef struct Database Database;

int  database_open (const char* path, Database** result);
void database_close (Database* database);
int  database_insert (Database* database, const DownloadTask* task);
int  database_update (Database* database, const DownloadTask* task);
int  database_load (Database* database, const char* uuid, DownloadTask* task);
int  database_list (Database* database, size_t offset, size_t limit, DownloadTask** tasks, size_t* count, size_t* total);
int  database_delete (Database* database, const char* uuid);
int  database_recover (Database* database);
int  database_clear_completed (Database* database);

#endif
