#ifndef DOWNLOADER_FILESYSTEM_H
#define DOWNLOADER_FILESYSTEM_H

#include "task.h"

int filesystem_prepare (DownloadTask* task);
int filesystem_commit (const DownloadTask* task);

#endif
