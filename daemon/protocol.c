#include "protocol.h"

#include <errno.h>
#include <string.h>

static const ProtocolOps* gProtocols[DOWNLOADER_MAX_PROTOCOLS];
static size_t             gProtocolCount;

int protocol_register (const ProtocolOps* ops)
{
    size_t index;

    if (ops == NULL || ops->name == NULL || ops->canHandle == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; index < gProtocolCount; index++) {
        if (strcmp (gProtocols[index]->name, ops->name) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    if (gProtocolCount >= DOWNLOADER_MAX_PROTOCOLS) {
        errno = ENOSPC;
        return -1;
    }
    gProtocols[gProtocolCount++] = ops;
    return 0;
}

const ProtocolOps* protocol_find (const char* url, const char* protocol)
{
    size_t index;

    if (url == NULL || protocol == NULL) {
        errno = EINVAL;
        return NULL;
    }
    for (index = 0; index < gProtocolCount; index++) {
        if (gProtocols[index]->canHandle (url, protocol))
            return gProtocols[index];
    }
    errno = ENOTSUP;
    return NULL;
}
