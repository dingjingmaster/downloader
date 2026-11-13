#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "ipc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int handle_request (const char* data, size_t dataSize, char* response, size_t responseCapacity,
                           size_t* responseSize, void* userData)
{
    (void)userData;
    assert (dataSize == strlen (data));
    assert (strcmp (data, "{\"op\":\"ping\"}") == 0);
    assert (snprintf (response, responseCapacity, "{\"ok\":true}") > 0);
    *responseSize = strlen (response);
    return 0;
}

int main (void)
{
    char       rootPath[] = "/tmp/downloader-ipc-XXXXXX";
    char       socketPath[512];
    char*      root;
    Ipc*       ipc;
    CIpcClient* client;
    CIpcMessage response;
    char        responseData[128] = { 0 };

    root = mkdtemp (rootPath);
    assert (root != NULL);
    assert (snprintf (socketPath, sizeof (socketPath), "%s/parent/child/downloader.sock", root) > 0);
    ipc = ipc_new (socketPath, handle_request, NULL);
    assert (ipc != NULL);
    assert (ipc_start (ipc) == 0);
    client = c_ipc_client_new (socketPath);
    assert (client != NULL);
    assert (c_ipc_client_send (client, 1U, C_IPC_FLAG_REQUEST, "{\"op\":\"ping\"}", strlen ("{\"op\":\"ping\"}"),
                               &response, responseData,
                               sizeof (responseData)) == 0);
    assert (response.data_len == strlen ("{\"ok\":true}"));
    assert (strcmp (responseData, "{\"ok\":true}") == 0);
    c_ipc_client_free (client);
    ipc_stop (ipc);
    ipc_free (ipc);
    return 0;
}
