#ifndef DOWNLOADER_ARIA_H
#define DOWNLOADER_ARIA_H

#include <clib/macro.h>
#include "../daemon/protocol.h"

C_BEGIN_EXTERN_C

/* Borrowed process-lifetime operations. open only copies input; run owns all I/O.
 * tempPath must be a private, stable directory, unique to this task. Relative
 * paths are resolved in run; the process working directory must remain stable.
 * The core publishes/removes the directory. Its .downloader-aria child is
 * reserved for owned metadata. Symlink parents are allowed, but stage entries
 * must not be links or concurrently modified outside the worker.
 * run returns 0 on success, 1 on pause (controls retained), -1 with errno on
 * failure/cancel. Callbacks run on the worker, including while waiting for the
 * single-session lock; pause/cancel may be called from onProgress.
 * close must follow run, never race with it. All libaria2 users must be
 * serialized; engine self-checks must precede tasks. Join workers before exit. */
const ProtocolOps* aria_protocol_ops (void);

C_END_EXTERN_C

#endif
