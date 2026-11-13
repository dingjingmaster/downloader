#ifndef DOWNLOADER_DATABASE_H
#define DOWNLOADER_DATABASE_H

#include "task.h"

typedef struct Database Database;

int  database_open (const char* path, Database** result);
void database_close (Database* database);
int  database_insert (Database* database, const DownloadTask* task);
int  database_update (Database* database, const DownloadTask* task);
int  database_load (Database* database, const char* uuid, DownloadTask* task);
int  database_recover (Database* database);
int  database_clear_completed (Database* database);

#endif
