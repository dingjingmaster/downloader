#ifndef DOWNLOADER_TASK_MANAGER_H
#define DOWNLOADER_TASK_MANAGER_H

#include "database.h"
#include "scheduler.h"

#include <stddef.h>
#include <clib/macro.h>

C_BEGIN_EXTERN_C

typedef struct TaskManager TaskManager;
int task_manager_set_download_directory (TaskManager* manager, const char* directory);
const char* task_manager_download_directory (const TaskManager* manager);
int task_manager_execute (TaskManager* manager, DownloadTask* task);

TaskManager* task_manager_new (Database* database, Scheduler* scheduler);
void         task_manager_free (TaskManager* manager);
/* Request workers to pause before scheduler_stop joins them. */
int          task_manager_stop (TaskManager* manager);
int          task_manager_create (TaskManager* manager, const char* resourceUrl, const char* fileName,
                                  const char* targetDirectory, const char* protocol, const char* extraJson,
                                  DownloadTask* result);
int          task_manager_delete (TaskManager* manager, const char* uuid);
int          task_manager_start (TaskManager* manager, const char* uuid);
int          task_manager_pause (TaskManager* manager, const char* uuid);
int          task_manager_resume (TaskManager* manager, const char* uuid);
int          task_manager_get (TaskManager* manager, const char* uuid, DownloadTask* task);
int          task_manager_list (TaskManager* manager, size_t page, size_t pageSize, DownloadTask** tasks, size_t* count,
                                size_t* total);
int          task_manager_clear_completed (TaskManager* manager, size_t* deleted);

C_END_EXTERN_C

#endif
