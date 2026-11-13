#ifndef DOWNLOADER_IPC_H
#define DOWNLOADER_IPC_H

#include <clib/ipc.h>

#include <stddef.h>

typedef struct Ipc Ipc;
typedef int (*IpcRequestFunc) (const char* data, size_t dataSize, char* response, size_t responseCapacity,
                               size_t* responseSize, void* userData);

Ipc* ipc_new (const char* socketPath, IpcRequestFunc requestFunc, void* userData);
int  ipc_start (Ipc* ipc);
void ipc_stop (Ipc* ipc);
void ipc_free (Ipc* ipc);

#endif
