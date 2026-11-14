#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "web.h"

#include <clib/memory.h>
#include <clib/path.h>
#include <clib/string-util.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_REQUEST_CAPACITY (1024U * 1024U)
#define WEB_HEADER_CAPACITY  (64U * 1024U)

struct Web
{
    char*              listen;
    char*              documentRoot;
    int32_t            port;
    int                listenFd;
    volatile bool      stopRequested;
    CThread*           thread;
    WebRequestHandler  requestHandler;
    void*              userData;
};

static int web_set_nonblocking (int fd)
{
    int flags = fcntl (fd, F_GETFL, 0);

    if (flags < 0 || fcntl (fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -1;
    return 0;
}

static int web_open_listener (Web* web)
{
    char              portText[16];
    struct addrinfo   hints;
    struct addrinfo*  addresses;
    struct addrinfo*  address;
    int               result;
    int               fd = -1;
    int               reuse = 1;

    memset (&hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    (void)snprintf (portText, sizeof (portText), "%d", web->port);
    result = getaddrinfo (web->listen, portText, &hints, &addresses);
    if (result != 0) {
        errno = EINVAL;
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        fd = socket (address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0)
            continue;
        if (setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse)) != 0 ||
            bind (fd, address->ai_addr, address->ai_addrlen) != 0 || listen (fd, 32) != 0 || web_set_nonblocking (fd) != 0) {
            int savedErrno = errno;
            (void)close (fd);
            fd = -1;
            errno = savedErrno;
            continue;
        }
        break;
    }
    freeaddrinfo (addresses);
    if (fd < 0)
        return -1;
    web->listenFd = fd;
    return 0;
}

static int web_status_reason (int status, const char** reason)
{
    switch (status) {
    case 200:
        *reason = "OK";
        return 0;
    case 201:
        *reason = "Created";
        return 0;
    case 400:
        *reason = "Bad Request";
        return 0;
    case 401:
        *reason = "Unauthorized";
        return 0;
    case 403:
        *reason = "Forbidden";
        return 0;
    case 404:
        *reason = "Not Found";
        return 0;
    case 405:
        *reason = "Method Not Allowed";
        return 0;
    case 409:
        *reason = "Conflict";
        return 0;
    case 413:
        *reason = "Payload Too Large";
        return 0;
    case 500:
        *reason = "Internal Server Error";
        return 0;
    case 502:
        *reason = "Bad Gateway";
        return 0;
    case 503:
        *reason = "Service Unavailable";
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

static int web_send_response (int clientFd, const WebResponse* response)
{
    char        header[512];
    const char* reason;
    const char* contentType = response->contentType == NULL ? "application/json" : response->contentType;
    size_t      bodySize = response->body == NULL ? 0 : response->bodySize;
    int         headerSize;
    size_t      sent = 0;

    if (web_status_reason (response->status, &reason) != 0)
        return -1;
    headerSize = snprintf (header, sizeof (header),
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n",
                           response->status, reason, contentType, bodySize);
    if (headerSize < 0 || (size_t)headerSize >= sizeof (header)) {
        errno = ENOSPC;
        return -1;
    }
    if (response->setCookie[0] != '\0') {
        int written = snprintf (header + headerSize, sizeof (header) - (size_t)headerSize, "Set-Cookie: %s\r\n",
                                response->setCookie);
        if (written < 0 || (size_t)written >= sizeof (header) - (size_t)headerSize) {
            errno = ENOSPC;
            return -1;
        }
        headerSize += written;
    }
    {
        int written = snprintf (header + headerSize, sizeof (header) - (size_t)headerSize,
                                "Connection: close\r\n\r\n");
        if (written < 0 || (size_t)written >= sizeof (header) - (size_t)headerSize) {
            errno = ENOSPC;
            return -1;
        }
        headerSize += written;
    }
    while (sent < (size_t)headerSize) {
        ssize_t written = send (clientFd, header + sent, (size_t)headerSize - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)written;
    }
    sent = 0;
    while (sent < bodySize) {
        ssize_t written = send (clientFd, response->body + sent, bodySize - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)written;
    }
    return 0;
}

static int web_path_is_traversal (const char* path)
{
    return path == NULL || strstr (path, "..") != NULL || strchr (path, '\\') != NULL;
}

static const char* web_content_type (const char* path)
{
    const char* extension = strrchr (path, '.');

    if (extension != NULL && strcmp (extension, ".html") == 0)
        return "text/html; charset=utf-8";
    if (extension != NULL && strcmp (extension, ".js") == 0)
        return "application/javascript; charset=utf-8";
    if (extension != NULL && strcmp (extension, ".css") == 0)
        return "text/css; charset=utf-8";
    return "application/octet-stream";
}

static int web_load_static (Web* web, const char* path, WebResponse* response)
{
    const char* resourceName;
    char*       resourcePath;
    char*       rootRealPath;
    char*       resourceRealPath;
    FILE*       file;
    long        fileSize;
    char*       body;
    size_t      readSize;

    if (web_path_is_traversal (path)) {
        response->status = 400;
        response->contentType = "application/json";
        response->body = "{\"ok\":false,\"error\":\"path_traversal\"}";
        response->bodySize = strlen (response->body);
        return 1;
    }
    if (strcmp (path, "/") == 0 || strcmp (path, "/index.html") == 0)
        resourceName = "index.html";
    else if (strcmp (path, "/app.js") == 0)
        resourceName = "app.js";
    else if (strcmp (path, "/style.css") == 0)
        resourceName = "style.css";
    else
        return 0;
    rootRealPath = realpath (web->documentRoot, NULL);
    if (rootRealPath == NULL)
        rootRealPath = realpath ("web", NULL);
    resourcePath = rootRealPath == NULL ? NULL : c_path_join (rootRealPath, resourceName);
    resourceRealPath = resourcePath == NULL ? NULL : realpath (resourcePath, NULL);
    c_free (resourcePath);
    if (rootRealPath == NULL || resourceRealPath == NULL ||
        strncmp (rootRealPath, resourceRealPath, strlen (rootRealPath)) != 0 ||
        (resourceRealPath[strlen (rootRealPath)] != '\0' && resourceRealPath[strlen (rootRealPath)] != '/')) {
        free (rootRealPath);
        free (resourceRealPath);
        response->status = 404;
        response->contentType = "application/json";
        response->body = "{\"ok\":false,\"error\":\"not_found\"}";
        response->bodySize = strlen (response->body);
        return 1;
    }
    free (rootRealPath);
    file = fopen (resourceRealPath, "rb");
    free (resourceRealPath);
    if (file == NULL) {
        response->status = 404;
        response->contentType = "application/json";
        response->body = "{\"ok\":false,\"error\":\"not_found\"}";
        response->bodySize = strlen (response->body);
        return 1;
    }
    if (fseek (file, 0, SEEK_END) != 0 || (fileSize = ftell (file)) < 0 || fileSize > (long)WEB_REQUEST_CAPACITY ||
        fseek (file, 0, SEEK_SET) != 0) {
        (void)fclose (file);
        response->status = 500;
        response->contentType = "application/json";
        response->body = "{\"ok\":false,\"error\":\"resource_invalid\"}";
        response->bodySize = strlen (response->body);
        return 1;
    }
    body = (char*)c_malloc ((size_t)fileSize + 1U);
    if (body == NULL) {
        (void)fclose (file);
        return -1;
    }
    readSize = fread (body, 1U, (size_t)fileSize, file);
    (void)fclose (file);
    if (readSize != (size_t)fileSize) {
        c_free (body);
        response->status = 500;
        response->contentType = "application/json";
        response->body = "{\"ok\":false,\"error\":\"resource_read_failed\"}";
        response->bodySize = strlen (response->body);
        return 1;
    }
    body[fileSize] = '\0';
    response->status = 200;
    response->contentType = web_content_type (resourceName);
    response->body = body;
    response->bodySize = (size_t)fileSize;
    response->bodyOwned = true;
    return 1;
}

static char* web_find_header_end (char* request, size_t requestSize)
{
    size_t index;

    if (requestSize < 4U)
        return NULL;
    for (index = 0; index + 3U < requestSize; index++) {
        if (memcmp (request + index, "\r\n\r\n", 4U) == 0)
            return request + index;
    }
    return NULL;
}

/* One pass: terminating one header line must not hide later lines from a
 * second lookup. Values point into the request buffer. */
static void web_parse_auth_headers (char* headers, const char** cookie, const char** csrf)
{
    char* line = headers;

    *cookie = "";
    *csrf = "";
    while (line != NULL && *line != '\0') {
        char*  end = strstr (line, "\r\n");
        size_t lineSize = end == NULL ? strlen (line) : (size_t)(end - line);
        bool   cookieLine = lineSize > 6U && strncasecmp (line, "Cookie", 6U) == 0 && line[6] == ':';
        bool   csrfLine = lineSize > 12U && strncasecmp (line, "X-CSRF-Token", 12U) == 0 && line[12] == ':';
        if (cookieLine || csrfLine) {
            char* value = line + (cookieLine ? 7U : 13U);
            char* valueEnd = line + lineSize;
            while (*value == ' ' || *value == '\t')
                value++;
            while (valueEnd > value && (valueEnd[-1] == ' ' || valueEnd[-1] == '\t'))
                valueEnd--;
            *valueEnd = '\0';
            if (cookieLine)
                *cookie = value;
            else
                *csrf = value;
        }
        line = end == NULL ? NULL : end + 2;
    }
}

static int web_parse_content_length (const char* headers, size_t* contentLength)
{
    const char* marker = "Content-Length:";
    const char* line = headers;

    *contentLength = 0;
    while (line != NULL && *line != '\0') {
        const char* end = strstr (line, "\r\n");
        size_t       lineSize = end == NULL ? strlen (line) : (size_t)(end - line);
        if (lineSize >= strlen (marker) && strncasecmp (line, marker, strlen (marker)) == 0) {
            char*  valueEnd;
            char*  valueStart = (char*)line + strlen (marker);
            unsigned long long parsed;
            while (*valueStart == ' ' || *valueStart == '\t')
                valueStart++;
            errno = 0;
            parsed = strtoull (valueStart, &valueEnd, 10);
            if (errno != 0 || valueEnd == valueStart || (size_t)(valueEnd - line) > lineSize || parsed > WEB_REQUEST_CAPACITY)
                return -1;
            *contentLength = (size_t)parsed;
            return 0;
        }
        line = end == NULL ? NULL : end + 2;
    }
    return 0;
}

static int web_handle_client (Web* web, int clientFd)
{
    char           requestBuffer[WEB_REQUEST_CAPACITY + 1U];
    char*          headerEnd;
    char*          requestLineEnd;
    char*          method = NULL;
    char*          path = NULL;
    char*          version = NULL;
    char*          query;
    size_t         received = 0;
    size_t         contentLength;
    WebRequest     request;
    WebResponse    response = { .status = 500, .contentType = "application/json", .body = "{\"ok\":false}", .bodySize = 11U };
    int            result;

    while (received < WEB_HEADER_CAPACITY) {
        ssize_t count = recv (clientFd, requestBuffer + received, WEB_REQUEST_CAPACITY - received, 0);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return 0;
        received += (size_t)count;
        requestBuffer[received] = '\0';
        headerEnd = web_find_header_end (requestBuffer, received);
        if (headerEnd != NULL)
            break;
    }
    headerEnd = web_find_header_end (requestBuffer, received);
    if (headerEnd == NULL) {
        response.status = 400;
        response.body = "{\"ok\":false,\"error\":\"headers_too_large\"}";
        response.bodySize = strlen (response.body);
        return web_send_response (clientFd, &response);
    }
    *headerEnd = '\0';
    requestLineEnd = strstr (requestBuffer, "\r\n");
    if (requestLineEnd == NULL || sscanf (requestBuffer, "%ms %ms %ms", &method, &path, &version) != 3) {
        response.status = 400;
        response.body = "{\"ok\":false,\"error\":\"invalid_request\"}";
        response.bodySize = strlen (response.body);
        result = web_send_response (clientFd, &response);
        free (method);
        free (path);
        free (version);
        return result;
    }
    (void)version;
    query = strchr (path, '?');
    if (query != NULL) {
        *query = '\0';
        query++;
    }
    if (web_parse_content_length (requestLineEnd + 2, &contentLength) != 0 || contentLength > WEB_REQUEST_CAPACITY - received) {
        response.status = 400;
        response.body = "{\"ok\":false,\"error\":\"invalid_body\"}";
        response.bodySize = strlen (response.body);
        result = web_send_response (clientFd, &response);
        free (method);
        free (path);
        free (version);
        return result;
    }
    while (received - (size_t)(headerEnd - requestBuffer) - 4U < contentLength) {
        ssize_t count = recv (clientFd, requestBuffer + received, WEB_REQUEST_CAPACITY - received, 0);
        if (count <= 0) {
            free (method);
            free (path);
            free (version);
            return -1;
        }
        received += (size_t)count;
    }
    request.method = method;
    request.path = path;
    request.query = query;
    request.body = headerEnd + 4;
    request.bodySize = contentLength;
    web_parse_auth_headers (requestLineEnd + 2, &request.cookie, &request.csrfToken);
    result = web_load_static (web, request.path, &response);
    if (result < 0) {
        free (method);
        free (path);
        free (version);
        return -1;
    }
    if (result == 1) {
        result = web_send_response (clientFd, &response);
        if (response.bodyOwned)
            c_free ((void*)response.body);
        free (method);
        free (path);
        free (version);
        return result;
    }
    if (web->requestHandler != NULL)
        result = web->requestHandler (&request, &response, web->userData);
    else
        result = 0;
    if (result != 0) {
        if (response.bodyOwned)
            c_free ((void*)response.body);
        response.status = 500;
        response.contentType = "application/json";
        response.body = "{\"ok\":false,\"error\":\"handler_failed\"}";
        response.bodySize = strlen (response.body);
        response.bodyOwned = false;
    }
    result = web_send_response (clientFd, &response);
    if (response.bodyOwned)
        c_free ((void*)response.body);
    free (method);
    free (path);
    free (version);
    return result;
}

static void web_thread (void* userData)
{
    Web* web = (Web*)userData;

    while (!web->stopRequested) {
        struct pollfd descriptor = { .fd = web->listenFd, .events = POLLIN };
        int pollResult = poll (&descriptor, 1, 100);
        if (pollResult <= 0)
            continue;
        if ((descriptor.revents & POLLIN) != 0) {
            int clientFd = accept (web->listenFd, NULL, NULL);
            if (clientFd >= 0) {
                (void)web_handle_client (web, clientFd);
                (void)close (clientFd);
            }
        }
    }
}

Web* web_new (const WebOptions* options, WebRequestHandler requestHandler, void* userData)
{
    Web* web;

    if (options == NULL || options->listen == NULL || options->listen[0] == '\0' || options->port < 1 ||
        options->port > 65535) {
        errno = EINVAL;
        return NULL;
    }
    web = (Web*)c_malloc0 (sizeof (*web));
    if (web == NULL)
        return NULL;
    web->listen = c_strdup (options->listen);
    web->documentRoot = c_strdup (options->documentRoot == NULL ? "web" : options->documentRoot);
    if (web->listen == NULL || web->documentRoot == NULL) {
        c_free (web->documentRoot);
        c_free (web->listen);
        c_free (web);
        return NULL;
    }
    web->port = options->port;
    web->listenFd = -1;
    web->requestHandler = requestHandler;
    web->userData = userData;
    return web;
}

int web_start (Web* web)
{
    if (web == NULL || web->thread != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (web_open_listener (web) != 0)
        return -1;
    web->stopRequested = false;
    web->thread = c_thread_new_named ("downloader-web", web_thread, web);
    if (web->thread == NULL) {
        int savedErrno = errno;
        (void)close (web->listenFd);
        web->listenFd = -1;
        errno = savedErrno;
        return -1;
    }
    return 0;
}

void web_stop (Web* web)
{
    if (web == NULL || web->thread == NULL)
        return;
    web->stopRequested = true;
    if (web->listenFd >= 0) {
        (void)shutdown (web->listenFd, SHUT_RDWR);
        (void)close (web->listenFd);
        web->listenFd = -1;
    }
    (void)c_thread_join (web->thread);
    (void)c_thread_free (web->thread);
    web->thread = NULL;
}

void web_free (Web* web)
{
    if (web == NULL)
        return;
    web_stop (web);
    if (web->listenFd >= 0)
        (void)close (web->listenFd);
    c_free (web->listen);
    c_free (web->documentRoot);
    c_free (web);
}
