#ifndef DOWNLOADER_MEDIA_H
#define DOWNLOADER_MEDIA_H

#include "protocol.h"
#include <clib/macro.h>

C_BEGIN_EXTERN_C

/* HTTP(S) HLS/DASH VOD, remuxed without transcoding to Matroska.
 * open allocates only. All callbacks and control operations belong to the same
 * worker thread. pause/cancel may be called reentrantly from callbacks.
 * Each run (including resume) truncates tempPath and resets progress to zero;
 * partial containers are never appended. Core owns final fsync and rename.
 * HTTP(S) AES-128 identity keys are supported. Live streams, SAMPLE-AES/DRM
 * and external local resources are rejected.
 * Best audio/video streams are retained; alternate renditions/subtitles are not.
 */
const ProtocolOps* media_protocol_ops (void);

C_END_EXTERN_C

#endif
