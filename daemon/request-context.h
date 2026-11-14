#ifndef DOWNLOADER_REQUEST_CONTEXT_H
#define DOWNLOADER_REQUEST_CONTEXT_H

#include "credential-store.h"
#include "task.h"

#include <clib/macro.h>

#include <stdbool.h>
#include <stddef.h>

C_BEGIN_EXTERN_C

typedef struct RequestContext RequestContext;

/* Resolution order: task extra fields, then the longest matching site profile,
 * then the configured defaults. A referenced profile that cannot be loaded is
 * an error; the caller must never fall back to a direct unauthenticated request. */
int request_context_resolve (RequestContext** result, const DownloadTask* task, CredentialStore* store,
                             const char* defaultAuthProfile, const char* defaultProxyProfile,
                             const char* globalProxyUrl, const char* globalProxyUsername,
                             const char* globalProxySecret);
void request_context_free (RequestContext* context);

bool                     request_context_has_auth (const RequestContext* context);
DownloaderCredentialKind request_context_auth_kind (const RequestContext* context);
const char*              request_context_auth_username (const RequestContext* context);
const char*              request_context_auth_secret (const RequestContext* context);
bool                     request_context_has_proxy (const RequestContext* context);
const char*              request_context_proxy_url (const RequestContext* context);
const char*              request_context_proxy_username (const RequestContext* context);
const char*              request_context_proxy_secret (const RequestContext* context);

C_END_EXTERN_C

#endif
