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
 * A dedicated engine thread owns the single libaria2 session and all GIDs;
 * download workers queue commands and wait for snapshots. run returns 0 on
 * success, 1 on pause (controls retained), -1 with errno on failure/cancel.
 * Callbacks run on the task's worker thread; pause/cancel may be called from
 * onProgress. close must follow run, never race with it. finish joins the
 * engine thread and must be called after every worker has returned. */
const ProtocolOps* aria_protocol_ops (void);

C_END_EXTERN_C

#endif
