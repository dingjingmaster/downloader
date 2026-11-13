#ifndef DOWNLOADER_FILESYSTEM_H
#define DOWNLOADER_FILESYSTEM_H

#include "task.h"
#include <clib/macro.h>

C_BEGIN_EXTERN_C

int filesystem_prepare (DownloadTask* task);
/* -1: not published, 0: durable, 1: published but parent-directory sync failed.
 * For result 1 errno carries the durability warning; never download it again. */
int filesystem_commit (const DownloadTask* task);
int filesystem_remove_partial (const DownloadTask* task);

C_END_EXTERN_C

#endif
