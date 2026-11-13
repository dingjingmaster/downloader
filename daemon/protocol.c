#include "protocol.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <clib/uri.h>
#include <clib/memory.h>

static bool protocol_path_ends_with (const char* path, size_t size, const char* suffix)
{
    size_t suffixSize = strlen (suffix);
    return size >= suffixSize && strncasecmp (path + size - suffixSize, suffix, suffixSize) == 0;
}

int protocol_detect (const char* url, char* protocol, size_t capacity)
{
    CUri uri;
    size_t errorOffset;
    size_t pathSize;
    char scheme[32];
    char* path = NULL;
    const char* detected;

    if (url == NULL || protocol == NULL || capacity == 0 ||
        c_uri_parse (c_string_view_from_cstr (url), &uri, &errorOffset) != 0 ||
        uri.scheme.size == 0 || uri.scheme.size >= sizeof (scheme)) {
        errno = EINVAL;
        return -1;
    }
    memcpy (scheme, uri.scheme.data, uri.scheme.size);
    scheme[uri.scheme.size] = '\0';
    for (char* letter = scheme; *letter; letter++) {
        if (*letter >= 'A' && *letter <= 'Z')
            *letter += 'a' - 'A';
    }
    if (c_uri_unescape_string (uri.path, &path, &pathSize) != 0)
        return -1;
    detected = scheme;
    if (strcmp (scheme, "hls+http") == 0 || strcmp (scheme, "hls+https") == 0)
        detected = "hls";
    else if (strcmp (scheme, "dash+http") == 0 || strcmp (scheme, "dash+https") == 0)
        detected = "dash";
    else if (strcmp (scheme, "http") == 0 || strcmp (scheme, "https") == 0 || strcmp (scheme, "file") == 0) {
        if (protocol_path_ends_with (path, pathSize, ".torrent"))
            detected = "bittorrent";
        else if (protocol_path_ends_with (path, pathSize, ".meta4") || protocol_path_ends_with (path, pathSize, ".metalink"))
            detected = "metalink";
        else if (strcmp (scheme, "file") != 0 && protocol_path_ends_with (path, pathSize, ".m3u8"))
            detected = "hls";
        else if (strcmp (scheme, "file") != 0 && protocol_path_ends_with (path, pathSize, ".mpd"))
            detected = "dash";
        else if (strcmp (scheme, "file") == 0)
            detected = NULL;
    } else if (strcmp (scheme, "ftp") != 0 && strcmp (scheme, "ftps") != 0 &&
               strcmp (scheme, "sftp") != 0 && strcmp (scheme, "smb") != 0 && strcmp (scheme, "magnet") != 0) {
        detected = NULL;
    }
    c_free (path);
    if (detected == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (snprintf (protocol, capacity, "%s", detected) >= (int)capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

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
