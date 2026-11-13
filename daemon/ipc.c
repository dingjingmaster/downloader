#include "ipc.h"

#include <clib/directory.h>
#include <clib/memory.h>
#include <clib/path.h>

#include <errno.h>
#include <string.h>

#define IPC_RESPONSE_CAPACITY (1024U * 1024U)

struct Ipc
{
    CIpcServer*   server;
    IpcRequestFunc requestFunc;
    void*         userData;
};

static int ipc_message (CIpcServer* server, int clientFd, const CIpcMessage* request, void* userData)
{
    Ipc*   ipc = (Ipc*)userData;
    char*  requestData;
    char*  response;
    size_t responseSize = 0;
    int    result;

    if (request == NULL || request->data == NULL || request->data_len == 0) {
        errno = EPROTO;
        return -1;
    }
    requestData = (char*)c_malloc (request->data_len + 1U);
    response = (char*)c_malloc (IPC_RESPONSE_CAPACITY);
    if (requestData == NULL || response == NULL) {
        c_free (requestData);
        c_free (response);
        return -1;
    }
    memcpy (requestData, request->data, request->data_len);
    requestData[request->data_len] = '\0';
    result = ipc->requestFunc (requestData, request->data_len, response, IPC_RESPONSE_CAPACITY,
                               &responseSize, ipc->userData);
    if (result == 0)
        result = c_ipc_server_reply (clientFd, request->type, response, responseSize);
    c_free (response);
    c_free (requestData);
    (void)server;
    return result;
}

Ipc* ipc_new (const char* socketPath, IpcRequestFunc requestFunc, void* userData)
{
    Ipc*  ipc;
    char* socketDirectory;
    int   savedErrno;

    if (socketPath == NULL || requestFunc == NULL) {
        errno = EINVAL;
        return NULL;
    }
    socketDirectory = c_path_dirname (socketPath);
    if (socketDirectory == NULL)
        return NULL;
    if (c_directory_create_parents_with_mode (socketDirectory, 0755U) != 0) {
        savedErrno = errno;
        c_free (socketDirectory);
        errno = savedErrno;
        return NULL;
    }
    c_free (socketDirectory);
    ipc = (Ipc*)c_malloc0 (sizeof (*ipc));
    if (ipc == NULL)
        return NULL;
    ipc->requestFunc = requestFunc;
    ipc->userData = userData;
    ipc->server = c_ipc_server_new (socketPath, 0U, ipc_message, ipc);
    if (ipc->server == NULL) {
        c_free (ipc);
        return NULL;
    }
    if (c_ipc_server_set_socket_mode (ipc->server, 0600U) != 0) {
        c_ipc_server_free (ipc->server);
        c_free (ipc);
        return NULL;
    }
    return ipc;
}

int ipc_start (Ipc* ipc)
{
    if (ipc == NULL) {
        errno = EINVAL;
        return -1;
    }
    return c_ipc_server_start (ipc->server);
}

void ipc_stop (Ipc* ipc)
{
    if (ipc != NULL)
        c_ipc_server_stop (ipc->server);
}

void ipc_free (Ipc* ipc)
{
    if (ipc == NULL)
        return;
    c_ipc_server_free (ipc->server);
    c_free (ipc);
}
