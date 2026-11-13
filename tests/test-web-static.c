#define _POSIX_C_SOURCE 200809L

#include "web.h"

#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int handle_not_found (const WebRequest* request, WebResponse* response, void* userData)
{
    (void)request;
    (void)userData;
    response->status = 404;
    response->contentType = "application/json";
    response->body = "{\"ok\":false,\"error\":\"not_found\"}";
    response->bodySize = strlen (response->body);
    return 0;
}

static void request_and_assert (int port, const char* path, const char* expectedStatus, const char* expectedBody)
{
    struct sockaddr_in address;
    char               request[512];
    char               response[8192];
    int                fd;
    ssize_t            received;

    fd = socket (AF_INET, SOCK_STREAM, 0);
    assert (fd >= 0);
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_port = htons ((uint16_t)port);
    assert (inet_pton (AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert (connect (fd, (struct sockaddr*)&address, sizeof (address)) == 0);
    (void)snprintf (request, sizeof (request), "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", path);
    assert (send (fd, request, strlen (request), 0) == (ssize_t)strlen (request));
    received = 0;
    response[0] = '\0';
    while (received < (ssize_t)sizeof (response) - 1 &&
           (strstr (response, expectedStatus) == NULL || strstr (response, expectedBody) == NULL)) {
        ssize_t count = recv (fd, response + received, sizeof (response) - 1U - (size_t)received, 0);
        assert (count > 0);
        received += count;
        response[received] = '\0';
    }
    assert (received > 0);
    assert (strstr (response, expectedStatus) != NULL);
    assert (strstr (response, expectedBody) != NULL);
    assert (close (fd) == 0);
}

int main (void)
{
    WebOptions options = { .listen = "127.0.0.1", .port = 38128, .documentRoot = "../web" };
    Web*        web = web_new (&options, handle_not_found, NULL);

    assert (web != NULL);
    assert (web_start (web) == 0);
    request_and_assert (options.port, "/", "200 OK", "Downloader");
    request_and_assert (options.port, "/app.js", "200 OK", "fetch");
    request_and_assert (options.port, "/missing.css", "404 Not Found", "not_found");
    request_and_assert (options.port, "/../CMakeLists.txt", "400 Bad Request", "path_traversal");
    web_stop (web);
    web_free (web);
    return 0;
}
