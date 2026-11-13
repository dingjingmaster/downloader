#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

extern const ProtocolOps* http_protocol_ops (void);

static bool can_handle_dummy (const char* url, const char* protocol)
{
    (void)url;
    return protocol != NULL && strcmp (protocol, "dummy") == 0;
}

int main (void)
{
    static const ProtocolOps dummy = {
        .name = "dummy",
        .canHandle = can_handle_dummy
    };
    const ProtocolOps* found;
    char detected[32];

    assert (protocol_detect ("https://host/movie.M3U8?token=abc", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "hls") == 0);
    assert (protocol_detect ("https://host/movie.mpd", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "dash") == 0);
    assert (protocol_detect ("http://host/file%2Etorrent#ignored", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "bittorrent") == 0);
    assert (protocol_detect ("https://host/file.meta4", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "metalink") == 0);
    assert (protocol_detect ("magnet:?xt=urn:btih:0123456789", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "magnet") == 0);
    assert (protocol_detect ("smb://host/share/file", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "smb") == 0);
    assert (protocol_detect ("dash+https://host/watch?id=1", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "dash") == 0);
    assert (protocol_detect ("https://host/file?name=foo.mpd", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "https") == 0);
    assert (protocol_detect ("file:///tmp/source.torrent", detected, sizeof (detected)) == 0);
    assert (strcmp (detected, "bittorrent") == 0);
    assert (protocol_detect ("file:///etc/passwd", detected, sizeof (detected)) != 0);
    assert (protocol_detect ("unknown://host", detected, sizeof (detected)) != 0);

    assert (protocol_register (&dummy) == 0);
    assert (protocol_register (&dummy) != 0);
    found = protocol_find ("dummy://resource", "dummy");
    assert (found == &dummy);
    found = protocol_find ("http://resource", "http");
    assert (found == NULL);
    assert (http_protocol_ops () != NULL);
    assert (http_protocol_ops ()->canHandle ("http://resource", "http"));
    assert (http_protocol_ops ()->canHandle ("https://resource", "https"));
    assert (http_protocol_ops ()->canHandle ("ftp://resource", "ftp"));
    assert (http_protocol_ops ()->canHandle ("sftp://resource", "sftp"));
    errno = 0;
    assert (protocol_find (NULL, NULL) == NULL);
    assert (errno == EINVAL);
    return 0;
}
