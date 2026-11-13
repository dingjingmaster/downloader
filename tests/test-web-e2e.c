#define _POSIX_C_SOURCE 200809L

#include <clib/ipc.h>
#include <clib/json.h>
#include <clib/memory.h>

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int http_request (int port, const char* method, const char* path, const char* body, char* response, size_t capacity)
{
    struct sockaddr_in address;
    char               request[4096];
    int                fd;
    int                requestSize;
    ssize_t            received;

    fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_port = htons ((uint16_t)port);
    (void)inet_pton (AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect (fd, (struct sockaddr*)&address, sizeof (address)) != 0) {
        (void)close (fd);
        return -1;
    }
    requestSize = snprintf (request, sizeof (request), "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                            method, path, body == NULL ? 0U : strlen (body), body == NULL ? "" : body);
    assert (requestSize > 0 && (size_t)requestSize < sizeof (request));
    assert (send (fd, request, (size_t)requestSize, 0) == requestSize);
    received = 0;
    while ((size_t)received < capacity - 1U) {
        ssize_t count = recv (fd, response + received, capacity - 1U - (size_t)received, 0);
        if (count <= 0)
            break;
        received += count;
    }
    response[received] = '\0';
    (void)close (fd);
    return received > 0 ? 0 : -1;
}

static void wait_for_web (int port)
{
    int attempts;
    char response[1024];

    for (attempts = 0; attempts < 100; attempts++) {
        if (http_request (port, "GET", "/api/tasks?page=1&page_size=20", NULL, response, sizeof (response)) == 0)
            return;
        nanosleep (&(struct timespec){ .tv_sec = 0, .tv_nsec = 10000000L }, NULL);
    }
    assert (!"Web server did not start");
}

int main (int argc, char* argv[])
{
    char          base[256];
    char          configPath[512];
    char          socketPath[512];
    char          databasePath[512];
    char          downloadDirectory[512];
    char          response[16384];
    FILE*         config;
    pid_t         child;
    CIpcClient*   client;
    CIpcMessage   ipcResponse;
    char          ipcBuffer[256];
    CJsonDocument* document;
    CJsonValue*   taskValue;
    CStringView   idView;
    char*         taskId;
    char          taskPath[1024];
    int           status;

    assert (argc == 2);
    (void)snprintf (base, sizeof (base), "/tmp/downloader-e2e-%ld", (long)getpid ());
    (void)snprintf (configPath, sizeof (configPath), "%s.ini", base);
    (void)snprintf (socketPath, sizeof (socketPath), "%s.sock", base);
    (void)snprintf (databasePath, sizeof (databasePath), "%s.db", base);
    (void)snprintf (downloadDirectory, sizeof (downloadDirectory), "%s-files", base);
    config = fopen (configPath, "w");
    assert (config != NULL);
    fprintf (config, "[daemon]\nipc_socket = %s\n[download]\ndatabase = %s\ndownload_dir = %s\n[web]\nenabled = true\nlisten = 0.0.0.0\nport = 38129\n",
             socketPath, databasePath, downloadDirectory);
    assert (fclose (config) == 0);
    child = fork ();
    assert (child >= 0);
    if (child == 0) {
        execl (argv[1], argv[1], "--config", configPath, (char*)NULL);
        _exit (127);
    }
    wait_for_web (38129);
    assert (http_request (38129, "GET", "/api/tasks?page=1&page_size=20", NULL, response, sizeof (response)) == 0);
    assert (strstr (response, "200 OK") != NULL && strstr (response, "\"tasks\":[]") != NULL);
    assert (http_request (38129, "POST", "/api/tasks", "{\"url\":\"https://example.test/e2e.bin\",\"file_name\":\"e2e.bin\",\"target_directory\":\"/tmp\",\"protocol\":\"https\"}", response, sizeof (response)) == 0);
    assert (strstr (response, "201 Created") != NULL);
    assert (strstr (response, "201 Created") != NULL);
    document = c_json_document_new_from_data (c_string_view_from_cstr (strstr (response, "{\"ok\":true")), C_JSON_PARSE_NONE, NULL);
    assert (document != NULL);
    taskValue = c_json_object_get (c_json_document_root (document), c_string_view_from_cstr ("task"));
    idView = c_json_value_get_string (c_json_object_get (taskValue, c_string_view_from_cstr ("id")));
    taskId = c_string_view_to_cstr (idView);
    assert (taskId != NULL);
    (void)snprintf (taskPath, sizeof (taskPath), "/api/tasks/%s/pause", taskId);
    assert (http_request (38129, "POST", taskPath, NULL, response, sizeof (response)) == 0);
    assert (strstr (response, "200 OK") != NULL);
    (void)snprintf (taskPath, sizeof (taskPath), "/api/tasks/%s", taskId);
    assert (http_request (38129, "DELETE", taskPath, NULL, response, sizeof (response)) == 0);
    assert (strstr (response, "200 OK") != NULL);
    c_free (taskId);
    c_json_document_free (document);
    client = c_ipc_client_new (socketPath);
    assert (client != NULL);
    assert (c_ipc_client_send (client, 1U, C_IPC_FLAG_REQUEST, "{\"op\":\"shutdown\"}", strlen ("{\"op\":\"shutdown\"}"),
                               &ipcResponse, ipcBuffer, sizeof (ipcBuffer)) == 0);
    c_ipc_client_free (client);
    assert (waitpid (child, &status, 0) == child);
    assert (WIFEXITED (status) && WEXITSTATUS (status) == 0);

    config = fopen (configPath, "w");
    assert (config != NULL);
    fprintf (config, "[daemon]\nipc_socket = %s\n[download]\ndatabase = %s\ndownload_dir = %s\n[web]\nenabled = false\nlisten = 0.0.0.0\nport = 38129\n",
             socketPath, databasePath, downloadDirectory);
    assert (fclose (config) == 0);
    child = fork ();
    assert (child >= 0);
    if (child == 0) {
        execl (argv[1], argv[1], "--config", configPath, (char*)NULL);
        _exit (127);
    }
    nanosleep (&(struct timespec){ .tv_sec = 0, .tv_nsec = 100000000L }, NULL);
    assert (http_request (38129, "GET", "/api/tasks?page=1&page_size=20", NULL, response, sizeof (response)) != 0);
    client = c_ipc_client_new (socketPath);
    assert (client != NULL);
    assert (c_ipc_client_send (client, 1U, C_IPC_FLAG_REQUEST, "{\"op\":\"shutdown\"}", strlen ("{\"op\":\"shutdown\"}"),
                               &ipcResponse, ipcBuffer, sizeof (ipcBuffer)) == 0);
    c_ipc_client_free (client);
    assert (waitpid (child, &status, 0) == child);
    assert (WIFEXITED (status) && WEXITSTATUS (status) == 0);
    (void)unlink (configPath);
    (void)unlink (socketPath);
    (void)unlink (databasePath);
    return 0;
}
