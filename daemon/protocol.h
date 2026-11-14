#ifndef DOWNLOADER_PROTOCOL_H
#define DOWNLOADER_PROTOCOL_H

#include "task.h"
#include <clib/macro.h>

#include <stdbool.h>
#include <stdint.h>

#define DOWNLOADER_MAX_PROTOCOLS 32U

C_BEGIN_EXTERN_C

typedef struct ProtocolTask ProtocolTask;
typedef struct ProtocolOps ProtocolOps;

typedef struct
{
    void (*onStarted) (void* userData);
    void (*onFileName) (void* userData, const char* name);
    void (*onProgress) (void* userData, uint64_t totalBytes, uint64_t downloadedBytes, uint64_t speedBytesSec);
    void (*onPaused) (void* userData);
    void (*onCompleted) (void* userData);
    void (*onFailed) (void* userData, int errorCode, const char* errorMessage);
    /* Persist validators before payload writes; -1 requests an abort. */
    int (*onMetadata) (void* userData, const char* etag, const char* lastModified);
    /* Versioned recovery checkpoint; referenced bytes must already be fsynced.
     * -1 requests an abort. */
    int (*onCheckpoint) (void* userData, const DownloadTaskCheckpoint* checkpoint);
    /* Scheduled retry starts at 1 (before its backoff); -1 requests an abort. */
    int (*onRetry) (void* userData, int attempt);
    /* Recording duration and detected gaps; called from the owning worker. */
    void (*onRecording) (void* userData, int64_t durationMillis, uint32_t gapCount);
} ProtocolCallbacks;

typedef struct
{
    char     resourceUrl[DOWNLOAD_TASK_URL_SIZE];
    char     fileName[DOWNLOAD_TASK_FILE_NAME_SIZE];
    /* Relative to the creation-time download root; empty means the root. */
    char     targetSubdirectory[DOWNLOAD_TASK_PATH_SIZE];
    char     extraJson[DOWNLOAD_TASK_EXTRA_JSON_SIZE];
    uint64_t totalBytes;
    char     checksumType[DOWNLOAD_TASK_PROTOCOL_SIZE];
    char     checksumValue[DOWNLOAD_TASK_CHECKSUM_SIZE];
} ProtocolItem;

typedef struct
{
    char code[64];
    char message[DOWNLOAD_TASK_ERROR_SIZE];
} ProtocolError;

struct ProtocolOps
{
    const char* name;
    bool (*canHandle) (const char* url, const char* protocol);
    int (*open) (ProtocolTask** result, const DownloadTask* task, const ProtocolCallbacks* callbacks, void* userData);
    int (*run) (ProtocolTask* task);
    int (*pause) (ProtocolTask* task);
    int (*resume) (ProtocolTask* task);
    int (*cancel) (ProtocolTask* task);
    /* Optional graceful finish for recording protocols (commit the result). */
    int (*stop) (ProtocolTask* task);
    void (*close) (ProtocolTask* task);
    /* Optional process-lifetime engine shutdown; call after every worker has
     * joined. NULL for adapters without a dedicated engine. */
    void (*finish) (void);
    /* Optional network-failure classification; true schedules an unlimited
     * manager retry. Called before close while the task context is alive. */
    bool (*retryable) (ProtocolTask* task, int errorCode, const char* message);
    /* Optional synchronous expansion: turn one submitted resource into concrete
     * per-file items. The request carries a resolved requestContext. items is
     * one c_malloc block owned by the core; error is zeroed by the caller. */
    int (*expand) (const DownloadTask* request, ProtocolItem** items, size_t* count,
                   char* groupName, size_t groupNameCapacity, ProtocolError* error);
};

int                 protocol_register (const ProtocolOps* ops);
const ProtocolOps*  protocol_find (const char* url, const char* protocol);
/* Infer the adapter from the scheme and decoded path, ignoring query/fragment. */
int                 protocol_detect (const char* url, char* protocol, size_t capacity);
/* Join process-lifetime protocol engines; call only after workers have stopped. */
void                protocol_finish_all (void);

C_END_EXTERN_C

#endif
