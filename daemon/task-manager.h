#ifndef DOWNLOADER_TASK_MANAGER_H
#define DOWNLOADER_TASK_MANAGER_H

#include "credential-store.h"
#include "database.h"
#include "scheduler.h"

#include <stddef.h>
#include <stdbool.h>
#include <clib/macro.h>

C_BEGIN_EXTERN_C

#define DOWNLOADER_CREATE_RESULT_TASKS 50U

typedef struct
{
    char          groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char          groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
    size_t        taskCount;
    size_t        skippedCount;
    bool          truncated;
    DownloadTask* tasks; /* Up to DOWNLOADER_CREATE_RESULT_TASKS entries; free with c_free(). */
    size_t        returnedCount;
    char          errorCode[64];
    char          errorMessage[DOWNLOAD_TASK_ERROR_SIZE];
} TaskCreateResult;

typedef struct TaskManager TaskManager;
int task_manager_set_download_directory (TaskManager* manager, const char* directory);
int task_manager_set_transfer_options (TaskManager* manager, int maxRetries, int connectTimeout, int transferTimeout);
/* Borrows the store and copies the fallback policy; call before starting tasks. */
int task_manager_set_request_policy (TaskManager* manager, CredentialStore* store, const char* defaultAuthProfile,
                                     const char* defaultProxyProfile, const char* proxyUrl,
                                     const char* proxyUsername, const char* proxyPassword);
const char* task_manager_download_directory (const TaskManager* manager);
CredentialStore* task_manager_credential_store (const TaskManager* manager);
int task_manager_execute (TaskManager* manager, DownloadTask* task);

TaskManager* task_manager_new (Database* database, Scheduler* scheduler);
void         task_manager_free (TaskManager* manager);
/* Request workers to pause before scheduler_stop joins them. */
int          task_manager_stop (TaskManager* manager);
int          task_manager_create (TaskManager* manager, const char* resourceUrl, const char* fileName,
                                  const char* targetDirectory, const char* protocol, const char* extraJson,
                                  DownloadTask* result);
/* Creates one task per expanded item, all-or-nothing. Fills result->errorCode on failure.
 * result->tasks is allocated with c_malloc and must be freed by the caller. */
int          task_manager_create_expanded (TaskManager* manager, const char* resourceUrl, const char* fileName,
                                           const char* targetDirectory, const char* extraJson,
                                           TaskCreateResult* result);
int          task_manager_delete (TaskManager* manager, const char* uuid);
int          task_manager_start (TaskManager* manager, const char* uuid);
int          task_manager_pause (TaskManager* manager, const char* uuid);
int          task_manager_resume (TaskManager* manager, const char* uuid);
/* Ask a recording adapter to finish and commit; ENOTSUP if unsupported. */
int          task_manager_finish (TaskManager* manager, const char* uuid);
int          task_manager_get (TaskManager* manager, const char* uuid, DownloadTask* task);
int          task_manager_list (TaskManager* manager, size_t page, size_t pageSize, DownloadTask** tasks, size_t* count,
                                size_t* total);
/* Start every queued task (including tasks recovered after a crash). */
int          task_manager_resume_queued (TaskManager* manager);
int          task_manager_clear_completed (TaskManager* manager, size_t* deleted);

typedef enum
{
    DOWNLOAD_GROUP_ACTION_START,
    DOWNLOAD_GROUP_ACTION_PAUSE,
    DOWNLOAD_GROUP_ACTION_RESUME,
    DOWNLOAD_GROUP_ACTION_DELETE
} DownloadGroupAction;

/* Applies one action to every task in a group; *affected receives the successes. */
int          task_manager_group_action (TaskManager* manager, const char* groupUuid, DownloadGroupAction action,
                                        size_t* affected);
int          task_manager_group_summary (TaskManager* manager, const char* groupUuid,
                                         DownloadTaskGroupSummary* summary);

C_END_EXTERN_C

#endif
