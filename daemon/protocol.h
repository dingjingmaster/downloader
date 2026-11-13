#ifndef DOWNLOADER_PROTOCOL_H
#define DOWNLOADER_PROTOCOL_H

#include "task.h"

#include <stdbool.h>
#include <stdint.h>

#define DOWNLOADER_MAX_PROTOCOLS 32U

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
} ProtocolCallbacks;

struct ProtocolOps
{
    const char* name;
    bool (*canHandle) (const char* url, const char* protocol);
    int (*open) (ProtocolTask** result, const DownloadTask* task, const ProtocolCallbacks* callbacks, void* userData);
    int (*run) (ProtocolTask* task);
    int (*pause) (ProtocolTask* task);
    int (*resume) (ProtocolTask* task);
    int (*cancel) (ProtocolTask* task);
    void (*close) (ProtocolTask* task);
};

int                 protocol_register (const ProtocolOps* ops);
const ProtocolOps*  protocol_find (const char* url, const char* protocol);

#endif
