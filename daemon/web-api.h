#ifndef DOWNLOADER_WEB_API_H
#define DOWNLOADER_WEB_API_H

#include "task-manager.h"
#include "web.h"

int web_api_handle (TaskManager* manager, const WebRequest* request, WebResponse* response);

#endif
