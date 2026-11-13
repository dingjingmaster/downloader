#ifndef DOWNLOADER_WEB_H
#define DOWNLOADER_WEB_H

#include <clib/thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Web Web;

typedef struct
{
    const char* listen;
    int32_t     port;
    const char* documentRoot;
} WebOptions;

typedef struct
{
    const char* method;
    const char* path;
    const char* query;
    const char* body;
    size_t      bodySize;
} WebRequest;

typedef struct
{
    int         status;
    const char* contentType;
    const char* body;
    size_t      bodySize;
    bool        bodyOwned;
} WebResponse;

typedef int (*WebRequestHandler) (const WebRequest* request, WebResponse* response, void* userData);

Web* web_new (const WebOptions* options, WebRequestHandler requestHandler, void* userData);
int  web_start (Web* web);
void web_stop (Web* web);
void web_free (Web* web);

#endif
