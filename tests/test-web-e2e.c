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

static int http_request_ex (int port, const char* method, const char* path, const char* body,
                            const char* extraHeaders, char* response, size_t capacity)
{
    struct sockaddr_in address;
    char               request[8192];
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
    requestSize = snprintf (request, sizeof (request),
                            "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                            "Content-Length: %zu\r\n%sConnection: close\r\n\r\n%s",
                            method, path, body == NULL ? 0U : strlen (body),
                            extraHeaders == NULL ? "" : extraHeaders, body == NULL ? "" : body);
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

static int http_request (int port, const char* method, const char* path, const char* body, char* response, size_t capacity)
{
    return http_request_ex (port, method, path, body, NULL, response, capacity);
}

static void extract_value (const char* response, const char* marker, char* destination, size_t capacity)
{
    const char* start = strstr (response, marker);
    const char* end;
    size_t      length;

    assert (start != NULL);
    start += strlen (marker);
    end = strpbrk (start, "\r\n;\"");
    assert (end != NULL);
    length = (size_t)(end - start);
    assert (length < capacity);
    memcpy (destination, start, length);
    destination[length] = '\0';
}

static int reserve_port (void)
{
    struct sockaddr_in address;
    socklen_t          length = sizeof (address);
    int                fd;
    int                port;

    fd = socket (AF_INET, SOCK_STREAM, 0);
    assert (fd >= 0);
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    address.sin_port = htons (0);
    assert (bind (fd, (struct sockaddr*)&address, sizeof (address)) == 0);
    assert (getsockname (fd, (struct sockaddr*)&address, &length) == 0);
    port = (int)ntohs (address.sin_port);
    (void)close (fd);
    assert (port > 0);
    return port;
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
    int           port;

    assert (argc == 2);
    port = reserve_port ();
    (void)snprintf (base, sizeof (base), "/tmp/downloader-e2e-%ld", (long)getpid ());
    (void)snprintf (configPath, sizeof (configPath), "%s.ini", base);
    (void)snprintf (socketPath, sizeof (socketPath), "%s.sock", base);
    (void)snprintf (databasePath, sizeof (databasePath), "%s.db", base);
    (void)snprintf (downloadDirectory, sizeof (downloadDirectory), "%s-files", base);
    config = fopen (configPath, "w");
    assert (config != NULL);
    fprintf (config, "[daemon]\nipc_socket = %s\n[download]\ndatabase = %s\ndownload_dir = %s\n[web]\nenabled = true\nlisten = 0.0.0.0\nport = %d\n",
             socketPath, databasePath, downloadDirectory, port);
    assert (fclose (config) == 0);
    child = fork ();
    assert (child >= 0);
    if (child == 0) {
        execl (argv[1], argv[1], "--config", configPath, (char*)NULL);
        _exit (127);
    }
    wait_for_web (port);
    assert (http_request (port, "GET", "/api/tasks?page=1&page_size=20", NULL, response, sizeof (response)) == 0);
    assert (strstr (response, "200 OK") != NULL && strstr (response, "\"tasks\":[]") != NULL);
    assert (http_request (port, "POST", "/api/tasks", "{\"url\":\"https://example.test/e2e.bin\",\"file_name\":\"e2e.bin\",\"target_directory\":\"/tmp\",\"protocol\":\"https\"}", response, sizeof (response)) == 0);
    assert (strstr (response, "201 Created") != NULL);
    assert (strstr (response, "201 Created") != NULL);
    document = c_json_document_new_from_data (c_string_view_from_cstr (strstr (response, "{\"ok\":true")), C_JSON_PARSE_NONE, NULL);
    assert (document != NULL);
    taskValue = c_json_object_get (c_json_document_root (document), c_string_view_from_cstr ("task"));
    idView = c_json_value_get_string (c_json_object_get (taskValue, c_string_view_from_cstr ("id")));
    taskId = c_string_view_to_cstr (idView);
    assert (taskId != NULL);
    (void)snprintf (taskPath, sizeof (taskPath), "/api/tasks/%s/pause", taskId);
    assert (http_request (port, "POST", taskPath, NULL, response, sizeof (response)) == 0);
    assert (strstr (response, "200 OK") != NULL);
    (void)snprintf (taskPath, sizeof (taskPath), "/api/tasks/%s", taskId);
    assert (http_request (port, "DELETE", taskPath, NULL, response, sizeof (response)) == 0);
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
    fprintf (config, "[daemon]\nipc_socket = %s\n[download]\ndatabase = %s\ndownload_dir = %s\n[web]\nenabled = false\nlisten = 0.0.0.0\nport = %d\n",
             socketPath, databasePath, downloadDirectory, port);
    assert (fclose (config) == 0);
    child = fork ();
    assert (child >= 0);
    if (child == 0) {
        execl (argv[1], argv[1], "--config", configPath, (char*)NULL);
        _exit (127);
    }
    nanosleep (&(struct timespec){ .tv_sec = 0, .tv_nsec = 100000000L }, NULL);
    assert (http_request (port, "GET", "/api/tasks?page=1&page_size=20", NULL, response, sizeof (response)) != 0);
    client = c_ipc_client_new (socketPath);
    assert (client != NULL);
    assert (c_ipc_client_send (client, 1U, C_IPC_FLAG_REQUEST, "{\"op\":\"shutdown\"}", strlen ("{\"op\":\"shutdown\"}"),
                               &ipcResponse, ipcBuffer, sizeof (ipcBuffer)) == 0);
    c_ipc_client_free (client);
    assert (waitpid (child, &status, 0) == child);
    assert (WIFEXITED (status) && WEXITSTATUS (status) == 0);
    config = fopen (configPath, "w");
    assert (config != NULL);
    fprintf (config, "[daemon]\nipc_socket = %s\n[download]\ndatabase = %s\ndownload_dir = %s\n"
                     "[web]\nenabled = true\nlisten = 0.0.0.0\nport = %d\nusername = admin\npassword = secret\n",
             socketPath, databasePath, downloadDirectory, port);
    assert (fclose (config) == 0);
    child = fork ();
    assert (child >= 0);
    if (child == 0) {
        execl (argv[1], argv[1], "--config", configPath, (char*)NULL);
        _exit (127);
    }
    wait_for_web (port);
    assert (http_request (port, "GET", "/api/tasks?page=1&page_size=20", NULL, response, sizeof (response)) == 0);
    assert (strstr (response, "401 Unauthorized") != NULL);
    assert (http_request (port, "POST", "/api/login", "{\"username\":\"admin\",\"password\":\"wrong\"}",
                          response, sizeof (response)) == 0);
    assert (strstr (response, "401 Unauthorized") != NULL);
    assert (http_request (port, "POST", "/api/login", "{\"username\":\"admin\",\"password\":\"secret\"}",
                          response, sizeof (response)) == 0);
    assert (strstr (response, "200 OK") != NULL);
    {
        char cookie[256];
        char csrf[128];
        char headers[512];
        char loginBody[1024];
        extract_value (response, "Set-Cookie: ", cookie, sizeof (cookie));
        extract_value (response, "\"csrf_token\":\"", csrf, sizeof (csrf));
        snprintf (headers, sizeof (headers), "Cookie: %s\r\n", cookie);
        assert (http_request_ex (port, "GET", "/api/tasks?page=1&page_size=20", NULL, headers, response,
                                 sizeof (response)) == 0);
        assert (strstr (response, "200 OK") != NULL);
        snprintf (loginBody, sizeof (loginBody),
                  "{\"url\":\"https://example.test/auth.bin\",\"file_name\":\"auth.bin\","
                  "\"target_directory\":\"/tmp\"}");
        assert (http_request_ex (port, "POST", "/api/tasks", loginBody, headers, response,
                                 sizeof (response)) == 0);
        assert (strstr (response, "403 Forbidden") != NULL);
        snprintf (headers, sizeof (headers), "Cookie: %s\r\nX-CSRF-Token: %s\r\n", cookie, csrf);
        assert (http_request_ex (port, "POST", "/api/tasks", loginBody, headers, response,
                                 sizeof (response)) == 0);
        assert (strstr (response, "201 Created") != NULL);
        assert (http_request_ex (port, "POST", "/api/logout", NULL, headers, response, sizeof (response)) == 0);
        assert (strstr (response, "200 OK") != NULL);
        snprintf (headers, sizeof (headers), "Cookie: %s\r\n", cookie);
        assert (http_request_ex (port, "GET", "/api/tasks?page=1&page_size=20", NULL, headers, response,
                                 sizeof (response)) == 0);
        assert (strstr (response, "401 Unauthorized") != NULL);
    }
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
