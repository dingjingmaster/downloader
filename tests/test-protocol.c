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
