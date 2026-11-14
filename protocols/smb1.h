#ifndef DOWNLOADER_SMB1_H
#define DOWNLOADER_SMB1_H

#include "protocol.h"
#include <clib/macro.h>

C_BEGIN_EXTERN_C

/* Compatibility backend for SMB1-only servers, implemented with Samba's
 * libsmbclient. `smb1://` forces the NT1 dialect and never falls back to
 * SMB2/3 or anonymous/guest login. Each task owns a private libsmbclient
 * context; pause/cancel are checked between 64 KiB reads, so an in-flight
 * blocking read must return before the worker yields. Resume restarts the
 * file from zero because libsmbclient has no resumable range API. */
const ProtocolOps* smb1_protocol_ops (void);

C_END_EXTERN_C

#endif
