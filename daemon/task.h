#ifndef DOWNLOADER_TASK_H
#define DOWNLOADER_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOWNLOAD_TASK_UUID_SIZE             64U
#define DOWNLOAD_TASK_FILE_NAME_SIZE        512U
#define DOWNLOAD_TASK_PATH_SIZE             1024U
#define DOWNLOAD_TASK_URL_SIZE              4096U
#define DOWNLOAD_TASK_PROTOCOL_SIZE         32U
#define DOWNLOAD_TASK_CHECKSUM_SIZE         128U
#define DOWNLOAD_TASK_HTTP_VALUE_SIZE       512U
#define DOWNLOAD_TASK_ERROR_SIZE            512U
#define DOWNLOAD_TASK_EXTRA_JSON_SIZE       4096U
#define DOWNLOAD_TASK_CONTEXT_SIZE          128U
#define DOWNLOAD_TASK_GROUP_UUID_SIZE       64U
#define DOWNLOAD_TASK_GROUP_NAME_SIZE       256U
#define DOWNLOAD_TASK_CHECKPOINT_VERSION    1U

struct RequestContext;

typedef enum
{
    DOWNLOAD_TASK_STATE_CREATED,
    DOWNLOAD_TASK_STATE_QUEUED,
    DOWNLOAD_TASK_STATE_DOWNLOADING,
    DOWNLOAD_TASK_STATE_PAUSED,
    DOWNLOAD_TASK_STATE_COMPLETED,
    DOWNLOAD_TASK_STATE_FAILED,
    DOWNLOAD_TASK_STATE_CANCELLED
} DownloadTaskState;

/* Recovery information proven durable at the time it was persisted.
 * version 0 means no checkpoint (legacy task or reset requested). */
typedef struct
{
    uint32_t          version;
    uint64_t          durableBytes;
    char              effectiveUrl[DOWNLOAD_TASK_URL_SIZE];
    char              etag[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char              lastModified[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char              requestContext[DOWNLOAD_TASK_CONTEXT_SIZE];
} DownloadTaskCheckpoint;

typedef struct
{
    char              uuid[DOWNLOAD_TASK_UUID_SIZE];
    char              fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    char              targetDirectory[DOWNLOAD_TASK_PATH_SIZE];
    char              targetPath[DOWNLOAD_TASK_PATH_SIZE];
    char              tempPath[DOWNLOAD_TASK_PATH_SIZE];
    char              resourceUrl[DOWNLOAD_TASK_URL_SIZE];
    char              protocol[DOWNLOAD_TASK_PROTOCOL_SIZE];
    DownloadTaskState state;
    int                errorCode;
    char              errorMessage[DOWNLOAD_TASK_ERROR_SIZE];
    uint64_t          totalBytes;
    uint64_t          downloadedBytes;
    double            progress;
    uint64_t          speedBytesSec;
    int64_t           createdAt;
    int64_t           startedAt;
    int64_t           pausedAt;
    int64_t           completedAt;
    int64_t           updatedAt;
    int               retryCount;
    int               maxRetries;
    char              checksumType[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char              checksumValue[DOWNLOAD_TASK_CHECKSUM_SIZE];
    char              etag[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char              lastModified[DOWNLOAD_TASK_HTTP_VALUE_SIZE];
    char              extraJson[DOWNLOAD_TASK_EXTRA_JSON_SIZE];
    char              groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char              groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
    /* Runtime policy supplied by the manager; not additional database columns. */
    int               connectTimeout;
    int               transferTimeout;
    /* Recording metadata reported by live media adapters. */
    int64_t           durationMillis;
    uint32_t          gapCount;
    /* Recovery view loaded from task_checkpoints; not a task column. */
    DownloadTaskCheckpoint checkpoint;
    /* Runtime resolved auth/proxy policy; never persisted. */
    struct RequestContext* requestContext;
} DownloadTask;

typedef struct
{
    char     groupUuid[DOWNLOAD_TASK_GROUP_UUID_SIZE];
    char     groupName[DOWNLOAD_TASK_GROUP_NAME_SIZE];
    size_t   taskCount;
    size_t   completedCount;
    size_t   failedCount;
    uint64_t totalBytes;
    uint64_t downloadedBytes;
} DownloadTaskGroupSummary;

const char*         download_task_state_name (DownloadTaskState state);
DownloadTaskState   download_task_state_from_name (const char* name);
int                 download_task_set_state (DownloadTask* task, DownloadTaskState state);

#endif
