#ifndef DOWNLOADER_BAIDU_H
#define DOWNLOADER_BAIDU_H

#include "protocol.h"

#include <clib/macro.h>

C_BEGIN_EXTERN_C

const ProtocolOps* baidu_protocol_ops (void);

/* Process-wide settings; call once at startup before tasks run. */
int baidu_protocol_configure (const char* transferDirectory, int maxParallelTasks, int maxFiles,
                              int signCacheSeconds, const char* apiBase, const char* pcsBase);

C_END_EXTERN_C

#endif
