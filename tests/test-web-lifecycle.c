#define _POSIX_C_SOURCE 200809L

#include "web.h"

#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int handle_request (const WebRequest* request, WebResponse* response, void* userData)
{
    (void)userData;
    assert (strcmp (request->method, "GET") == 0);
    assert (strcmp (request->path, "/health") == 0);
    response->status = 200;
    response->contentType = "text/plain";
    response->body = "ok";
    response->bodySize = 2;
    return 0;
}

static int connect_to_port (int port)
{
    struct sockaddr_in address;
    int                fd;

    fd = socket (AF_INET, SOCK_STREAM, 0);
    assert (fd >= 0);
    memset (&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_port = htons ((uint16_t)port);
    assert (inet_pton (AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert (connect (fd, (struct sockaddr*)&address, sizeof (address)) == 0);
    return fd;
}

int main (void)
{
    WebOptions options = { .listen = "127.0.0.1", .port = 38127 };
    Web*        web;
    int         clientFd;
    char        response[1024];
    ssize_t     received;

    web = web_new (&options, handle_request, NULL);
    assert (web != NULL);
    assert (web_start (web) == 0);
    clientFd = connect_to_port (options.port);
    assert (send (clientFd, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n", 43U, 0) == 43);
    received = 0;
    response[0] = '\0';
    while (received < (ssize_t)sizeof (response) - 1 && strstr (response, "\r\n\r\nok") == NULL) {
        ssize_t count = recv (clientFd, response + received, sizeof (response) - 1U - (size_t)received, 0);
        assert (count > 0);
        received += count;
        response[received] = '\0';
    }
    assert (received > 0);
    assert (strstr (response, "200 OK") != NULL);
    assert (strstr (response, "\r\n\r\nok") != NULL);
    assert (close (clientFd) == 0);
    web_stop (web);
    web_free (web);
    return 0;
}
