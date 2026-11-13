#!/usr/bin/env python3
"""Standalone aria adapter edge cases, using only loopback fixtures.

Usage: python3 tests/test-aria-adapter.py [build-vendor]
Requires the bundled aria2 and clib libraries already built in that directory.
Compiles a temporary C++11 harness; neither root CMake nor the daemon is needed.
The sibling test-bittorrent.py supplies the local BEP 3 peer/tracker fixture.
"""
import hashlib
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile

HARNESS = r'''
#include "protocols/aria.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
static void failed (void*, int code, const char* message)
{
    std::fprintf (stderr, "adapter error %d: %s\n", code, message);
}
int main (int argc, char** argv)
{
    if (argc != 4)
        return 2;
    DownloadTask source = {};
    ProtocolCallbacks callbacks = {};
    callbacks.onFailed = failed;
    std::snprintf (source.resourceUrl, sizeof (source.resourceUrl), "%s", argv[1]);
    std::snprintf (source.protocol, sizeof (source.protocol), "%s", argv[2]);
    std::snprintf (source.tempPath, sizeof (source.tempPath), "%s", argv[3]);
    ProtocolTask* task = NULL;
    const ProtocolOps* operations = aria_protocol_ops ();
    if (operations->open (&task, &source, &callbacks, NULL) != 0)
        return 3;
    int result = operations->run (task);
    operations->close (task);
    return result == 0 ? 0 : 1;
}
'''


def main():
    repository = pathlib.Path(__file__).resolve().parents[1]
    build = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else repository / 'build-vendor').resolve()
    spec = importlib.util.spec_from_file_location('bt_fixture', repository / 'tests/test-bittorrent.py')
    peer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(peer)
    fixture = peer.Fixture()
    try:
        with tempfile.TemporaryDirectory(prefix='aria-adapter-') as directory:
            root = pathlib.Path(directory)
            source = root / 'harness.cpp'
            source.write_text(HARNESS)
            executable = root / 'adapter'
            subprocess.run([
                os.environ.get('CXX', 'c++'), '-std=c++11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(repository), '-I' + str(build / 'clib/include'),
                '-I' + str(build / '3thrd/installed/include'),
                str(repository / 'protocols/aria.cpp'), str(source),
                str(build / '3thrd/installed/lib/libaria2.a'), str(build / 'clib/libclib.a'),
                str(build / 'clib/thirdparty/pcre2/libclib-pcre2-8.a'),
                '-lz', '-lexpat', '-lssl', '-lcrypto', '-pthread', '-ldl', '-lm',
                '-o', str(executable)], check=True)

            def run(url, protocol, stage, success=True):
                result = subprocess.run([str(executable), url, protocol, str(stage)],
                                        capture_output=True, text=True, timeout=45)
                assert (result.returncode == 0) == success, result.stderr

            def files(stage):
                return {path.relative_to(stage).as_posix(): path.read_bytes()
                        for path in stage.rglob('*') if path.is_file()}

            def metalink(name, payload, url):
                return (f'<metalink xmlns="urn:ietf:params:xml:ns:metalink"><file name="{name}">'
                        f'<size>{len(payload)}</size><hash type="sha-256">'
                        f'{hashlib.sha256(payload).hexdigest()}</hash><url>{url}</url>'
                        '</file></metalink>').encode()

            payload = b'actual payload with control-looking suffix\n' * 4096
            fixture.documents['/payload.aria2'] = payload
            nested = metalink('payload.aria2', payload, fixture.base + '/payload.aria2')
            fixture.documents['/nested.meta4'] = nested
            fixture.documents['/root.meta4'] = metalink('nested.meta4', nested, fixture.base + '/nested.meta4')
            run(fixture.base + '/root.meta4', 'metalink', root / 'chain')
            assert files(root / 'chain') == {'payload.aria2': payload}
            print('PASS followedBy chain removes metadata and preserves payload.aria2', flush=True)

            invalid = b'not a metalink descriptor'
            fixture.documents['/invalid.meta4'] = invalid
            fixture.documents['/invalid-root.meta4'] = metalink(
                'invalid.meta4', invalid, fixture.base + '/invalid.meta4')
            run(fixture.base + '/invalid-root.meta4', 'metalink', root / 'invalid-chain', False)
            print('PASS malformed followed descriptor cannot complete as payload', flush=True)

            torrent, url = fixture.torrent('metaurl')
            descriptor = (f'<metalink xmlns="urn:ietf:params:xml:ns:metalink">'
                          f'<file name="metaurl.bin"><size>{len(torrent.payload)}</size>'
                          f'<metaurl mediatype="torrent">{url}</metaurl></file></metalink>')
            local = root / 'local.meta4'
            local.write_text(descriptor)
            (root / 'alias').symlink_to(root, target_is_directory=True)
            stage = os.path.relpath(root / 'alias/relative', os.getcwd())
            run(local.as_uri(), 'metalink', stage)
            assert files(root / 'relative') == torrent.expected
            print('PASS metaurl belongsTo, file:// metadata, relative path, symlink parent', flush=True)

            torrent = peer.Torrent('bundle', {'a': b'alpha' * 5000,
                                            'a.aria2': b'legitimate payload' * 5000}, True, False)
            fixture.torrents[torrent.info_hash] = torrent
            fixture.documents['/bundle.torrent'] = peer.bencode({
                b'announce': (fixture.base + '/announce').encode(), b'info': torrent.info})
            run(fixture.base + '/bundle.torrent', 'bittorrent', root / 'suffix')
            assert files(root / 'suffix') == torrent.expected
            print('PASS multi-file torrent preserves both a and a.aria2', flush=True)

            for title, xml in [
                ('empty', '<metalink xmlns="urn:ietf:params:xml:ns:metalink"/>'),
                ('traversal', descriptor.replace('name="metaurl.bin"', 'name="../outside.bin"'))
            ]:
                metadata = root / (title + '.meta4')
                metadata.write_text(xml)
                run(metadata.as_uri(), 'metalink', root / title, False)
            (root / 'unsafe').mkdir()
            (root / 'unsafe/link').symlink_to(local)
            run(local.as_uri(), 'metalink', root / 'unsafe', False)
            assert local.read_text() == descriptor
            assert not (root / 'outside.bin').exists()
            print('PASS empty metadata, traversal, and stage symlink rejected', flush=True)
            assert not fixture.errors, fixture.errors
    finally:
        fixture.close()


if __name__ == '__main__':
    main()
