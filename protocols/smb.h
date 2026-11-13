#ifndef DOWNLOADER_SMB_H
#define DOWNLOADER_SMB_H

#include <clib/macro.h>
#include "protocol.h"

C_BEGIN_EXTERN_C

/* Borrowed static operations. open allocates only; run owns network/file I/O.
 * pause/resume/cancel and callbacks run on the same worker as run.
 * run returns 0 complete, 1 paused, or -1 with errno (ECANCELED on cancel).
 * Credentials use NTLMSSP; absent credentials request anonymous/guest access.
 */
const ProtocolOps* smb_protocol_ops (void);

C_END_EXTERN_C

#endif
