#!/usr/bin/env python3
"""Loopback-only integration: python3 tests/test-bittorrent.py /abs/downloader.

No aria2 executable, third-party Python packages or public trackers are needed.
The tiny seed implements BEP 3 and the BEP 10/9 metadata exchange for magnets.
Importing this file only defines fixtures; it never launches the downloader.
"""

import hashlib
import http.server
import json
import os
import pathlib
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.sax.saxutils


PIECE_SIZE = 16384
TIMEOUT = 60


def bencode(value):
    if isinstance(value, int):
        return b'i' + str(value).encode() + b'e'
    if isinstance(value, bytes):
        return str(len(value)).encode() + b':' + value
    if isinstance(value, list):
        return b'l' + b''.join(bencode(item) for item in value) + b'e'
    if isinstance(value, dict):
        return b'd' + b''.join(bencode(key) + bencode(value[key])
                              for key in sorted(value)) + b'e'
    raise TypeError(type(value))


def bdecode_prefix(encoded, offset=0):
    """Return (value, end offset), allowing a raw BEP 9 payload after a dict."""
    token = encoded[offset:offset + 1]
    if token == b'i':
        end = encoded.index(b'e', offset)
        return int(encoded[offset + 1:end]), end + 1
    if token in (b'l', b'd'):
        result = [] if token == b'l' else {}
        offset += 1
        while encoded[offset:offset + 1] != b'e':
            key, offset = bdecode_prefix(encoded, offset)
            if token == b'd':
                value, offset = bdecode_prefix(encoded, offset)
                result[key] = value
            else:
                result.append(key)
        return result, offset + 1
    colon = encoded.index(b':', offset)
    end = colon + 1 + int(encoded[offset:colon])
    if end > len(encoded):
        raise ValueError('truncated bencoded string')
    return encoded[colon + 1:end], end


def receive(connection, length):
    chunks = bytearray()
    while len(chunks) < length:
        chunk = connection.recv(length - len(chunks))
        if not chunk:
            raise EOFError('peer disconnected')
        chunks.extend(chunk)
    return bytes(chunks)


def message(connection, identifier, payload=b''):
    connection.sendall(struct.pack('!I', len(payload) + 1) + bytes([identifier]) + payload)


class Torrent:
    def __init__(self, name, files, multi=False, blocked=False):
        self.files = files
        self.payload = b''.join(files.values())
        self.info = {b'name': name.encode(), b'piece length': PIECE_SIZE,
                     b'pieces': b''.join(hashlib.sha1(self.payload[i:i + PIECE_SIZE]).digest()
                                         for i in range(0, len(self.payload), PIECE_SIZE)),
                     b'private': 1}
        if multi:
            self.info[b'files'] = [{b'length': len(payload),
                                    b'path': [part.encode() for part in path.split('/')]}
                                   for path, payload in files.items()]
            self.expected = {name + '/' + path: payload for path, payload in files.items()}
        else:
            self.info[b'length'] = len(self.payload)
            self.expected = {name: self.payload}
        self.metadata = bencode(self.info)
        self.info_hash = hashlib.sha1(self.metadata).digest()
        self.release = threading.Event()
        self.first_block = threading.Event()
        self.metadata_sent = threading.Event()
        self.announced = threading.Event()
        self.lock = threading.Lock()
        if not blocked:
            self.release.set()


class Peer(socketserver.BaseRequestHandler):
    def handle(self):
        connection = self.request
        connection.settimeout(10)
        fixture = self.server.fixture
        try:
            handshake = receive(connection, 68)
            if handshake[:20] != b'\x13BitTorrent protocol':
                # aria2 first probes MSE encryption; closing lets it retry the
                # standard plaintext handshake supported by this local seed.
                return
            torrent = fixture.torrents.get(handshake[28:48])
            if torrent is None:
                return
            connection.sendall(b'\x13BitTorrent protocol' + b'\x00\x00\x00\x00\x00\x10\x00\x00'
                               + torrent.info_hash + b'-PY0001-123456789012')
            pieces = len(torrent.info[b'pieces']) // 20
            bits = bytearray((pieces + 7) // 8)
            for index in range(pieces):
                bits[index // 8] |= 1 << (7 - index % 8)
            message(connection, 5, bytes(bits))
            message(connection, 1)
            message(connection, 20, b'\x00' + bencode({b'm': {b'ut_metadata': 1},
                                                       b'metadata_size': len(torrent.metadata)}))
            remote_metadata = None
            while not fixture.stopping.is_set():
                length = struct.unpack('!I', receive(connection, 4))[0]
                if not length:
                    continue
                if length > 1024 * 1024:
                    raise ValueError('oversized peer message')
                packet = receive(connection, length)
                if packet[0] == 20 and len(packet) > 2:
                    header, end = bdecode_prefix(packet, 2)
                    if packet[1] == 0:
                        remote_metadata = header.get(b'm', {}).get(b'ut_metadata')
                    elif packet[1] == 1 and header.get(b'msg_type') == 0:
                        if not remote_metadata:
                            raise ValueError('metadata request before extension negotiation')
                        piece = header[b'piece']
                        block = torrent.metadata[piece * PIECE_SIZE:(piece + 1) * PIECE_SIZE]
                        message(connection, 20, bytes([remote_metadata]) + bencode({
                            b'msg_type': 1, b'piece': piece, b'total_size': len(torrent.metadata)}) + block)
                        torrent.metadata_sent.set()
                elif packet[0] == 6:
                    index, begin, count = struct.unpack('!III', packet[1:])
                    start = index * PIECE_SIZE + begin
                    if count > PIECE_SIZE or start + count > len(torrent.payload):
                        raise ValueError('invalid block request')
                    with torrent.lock:
                        first = not torrent.first_block.is_set()
                        if first:
                            message(connection, 7, packet[1:9] + torrent.payload[start:start + count])
                            torrent.first_block.set()
                    if not first:
                        while not torrent.release.wait(.05):
                            if fixture.stopping.is_set():
                                return
                        message(connection, 7, packet[1:9] + torrent.payload[start:start + count])
        except (EOFError, ConnectionError, socket.timeout, OSError):
            pass  # Pause/delete deliberately close sockets, including blocked requests.
        except Exception as error:
            fixture.errors.append(f'peer: {error!r}')


class PeerServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False


class HttpFixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        fixture = self.server.fixture
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == '/announce':
            # parse_qs on str would replace arbitrary bytes in the info hash.
            query = dict(urllib.parse.unquote_to_bytes(part).split(b'=', 1)
                         for part in parsed.query.replace('+', '%20').split('&') if '=' in part)
            torrent = fixture.torrents.get(query.get(b'info_hash'))
            if torrent is None:
                self.send_error(404)
                return
            torrent.announced.set()
            content = bencode({b'interval': 1, b'complete': 1, b'incomplete': 0,
                               b'peers': socket.inet_aton('127.0.0.1')
                               + struct.pack('!H', fixture.peer.server_address[1])})
        else:
            content = fixture.documents.get(parsed.path)
            if content is None:
                self.send_error(404)
                return
        offset = 0
        end = len(content) - 1
        ranged = self.headers.get('Range')
        if ranged:
            start_text, end_text = ranged.removeprefix('bytes=').split('-', 1)
            offset = int(start_text or 0)
            end = min(int(end_text) if end_text else end, end)
        self.send_response(206 if ranged else 200)
        self.send_header('Content-Length', str(end - offset + 1))
        self.send_header('Accept-Ranges', 'bytes')
        if ranged:
            self.send_header('Content-Range', f'bytes {offset}-{end}/{len(content)}')
        self.end_headers()
        try:
            self.wfile.write(content[offset:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


class Fixture:
    def __init__(self):
        self.torrents = {}
        self.documents = {}
        self.errors = []
        self.stopping = threading.Event()
        self.peer = PeerServer(('127.0.0.1', 0), Peer)
        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), HttpFixture)
        self.peer.fixture = self
        self.http.fixture = self
        self.base = f'http://127.0.0.1:{self.http.server_port}'
        self.threads = [threading.Thread(target=server.serve_forever, daemon=True)
                        for server in (self.peer, self.http)]
        for thread in self.threads:
            thread.start()

    def torrent(self, name, multi=False, blocked=False):
        # Non-piece-aligned file boundaries exercise multi-file piece assembly.
        files = ({'first.bin': b'first-payload\x00' * 4001,
                  'nested/second.bin': b'second-payload\xff' * 6007} if multi else
                 {name + '.bin': hashlib.sha256(name.encode()).digest() * 32769})
        torrent = Torrent(name if multi else name + '.bin', files, multi, blocked)
        self.torrents[torrent.info_hash] = torrent
        self.documents['/' + name + '.torrent'] = bencode({
            b'announce': (self.base + '/announce').encode(), b'info': torrent.info})
        return torrent, self.base + '/' + name + '.torrent'

    def magnet(self, torrent):
        return ('magnet:?xt=urn:btih:' + torrent.info_hash.hex()
                + '&tr=' + urllib.parse.quote(self.base + '/announce', safe=''))

    def close(self):
        self.stopping.set()
        for torrent in self.torrents.values():
            torrent.release.set()
        for server in (self.http, self.peer):
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)


class Integration:
    def __init__(self, binary, root, fixture):
        self.root = root
        self.downloads = root / 'downloads'
        self.downloads.mkdir()
        self.fixture = fixture
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        self.base = f'http://127.0.0.1:{port}/api/tasks'
        config = root / 'download.ini'
        config.write_text(f'[daemon]\nipc_socket={root}/ipc.sock\n'
                          f'[download]\ndatabase={root}/meta.db\ndownload_dir={self.downloads}\n'
                          f'max_concurrent_tasks=3\n[web]\nlisten=127.0.0.1\nport={port}\n')
        self.log = (root / 'daemon.log').open('w+')
        environment = {key: value for key, value in os.environ.items()
                       if key.lower() not in ('http_proxy', 'https_proxy', 'all_proxy')}
        environment.update(NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
        self.daemon = subprocess.Popen([binary, '--config', str(config)], env=environment,
                                       stdout=self.log, stderr=subprocess.STDOUT,
                                       start_new_session=True)

    def api(self, path='', body=None, method=None):
        request = urllib.request.Request(self.base + path,
                                         data=None if body is None else json.dumps(body).encode(),
                                         headers={'Content-Type': 'application/json'}, method=method)
        try:
            with self.opener.open(request, timeout=5) as response:
                content = response.read()
                return json.loads(content) if content else {}
        except urllib.error.HTTPError as error:
            raise AssertionError(f'{method or request.get_method()} {path}: '
                                 f'HTTP {error.code}: {error.read().decode(errors="replace")}') from error

    def wait(self, predicate, description, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            if self.daemon.poll() is not None:
                raise AssertionError(f'daemon exited: {self.daemon.returncode}; {description}')
            if self.fixture.errors:
                raise AssertionError(self.fixture.errors)
            last = predicate()
            if last:
                return last
            time.sleep(.05)
        raise AssertionError(f'timed out after {timeout}s: {description}; last={last!r}')

    def ready(self):
        def probe():
            try:
                self.api()
                return True
            except (OSError, urllib.error.URLError):
                return False
        self.wait(probe, 'daemon Web API startup', 10)

    def create(self, url, name=None):
        body = {'url': url, 'target_directory': str(self.downloads)}
        if name is not None:
            body['file_name'] = name
        task = self.api(body=body)['task']
        assert task.get('id') and task.get('file_name'), task
        return task

    def start(self, task):
        self.api('/' + task['id'] + '/start', {})

    def stage(self, task):
        return self.downloads / f'.{task["id"]}.part'

    def state(self, task, expected, timeout=TIMEOUT):
        last = {}
        def poll():
            last.update(self.api('/' + task['id'])['task'])
            if last['state'] in expected:
                return dict(last)
            if last['state'] in ('failed', 'completed'):
                raise AssertionError(f'expected {expected}; task={last}')
            return False
        try:
            return self.wait(poll, f'task {task["id"]} states {expected}', timeout)
        except AssertionError as error:
            raise AssertionError(f'{error}; task including error_message={last}') from error

    def verify(self, task, expected):
        current = self.state(task, ('completed',))
        final = self.downloads / current['file_name']
        assert final.is_dir(), f'completed payload must be directory: {final}; task={current}'
        actual = {path.relative_to(final).as_posix(): hashlib.sha1(path.read_bytes()).hexdigest()
                  for path in final.rglob('*') if path.is_file()}
        hashes = {name: hashlib.sha1(payload).hexdigest() for name, payload in expected.items()}
        assert actual == hashes, f'payload tree/hash mismatch: actual={actual}, expected={hashes}; {current}'
        assert not self.stage(task).exists(), f'stage remains after commit: {current}'
        return final

    def delete(self, task):
        self.api('/' + task['id'], method='DELETE')
        self.wait(lambda: not self.stage(task).exists(), f'deleted stage {task["id"]}', 10)
        # Detect workers recreating the stage after DELETE returns.
        time.sleep(.2)
        assert not self.stage(task).exists(), f'deleted stage recreated: {task}'

    def blocked(self, name):
        torrent, url = self.fixture.torrent(name, blocked=True)
        task = self.create(url, name + '-output')
        self.start(task)
        self.state(task, ('downloading',))
        self.wait(lambda: torrent.first_block.is_set() and self.stage(task).is_dir(),
                  f'first peer block and staging directory: {task}')
        return task, torrent

    def run(self):
        self.ready()
        for multi in (False, True):
            torrent, url = self.fixture.torrent('multi' if multi else 'single', multi=multi)
            task = self.create(url)
            self.start(task)
            self.verify(task, torrent.expected)
            assert torrent.announced.is_set() and torrent.first_block.is_set(), task
            print(f'PASS remote torrent {"multi" if multi else "single"}-file payload', flush=True)

        torrent, _ = self.fixture.torrent('local-source')
        local = self.root / 'local-source.torrent'
        local.write_bytes(self.fixture.documents['/local-source.torrent'])
        task = self.create(local.as_uri())
        self.start(task)
        self.verify(task, torrent.expected)
        print('PASS local torrent metadata and peer payload', flush=True)

        torrent, _ = self.fixture.torrent('magnet')
        task = self.create(self.fixture.magnet(torrent), 'magnet-output')
        self.start(task)
        self.verify(task, torrent.expected)
        assert torrent.metadata_sent.is_set(), 'magnet never requested metadata from peer'
        assert torrent.first_block.is_set(), 'magnet never retrieved peer payload'
        print('PASS magnet metadata exchange and payload', flush=True)

        expected = {'metalink-one.bin': b'metalink-one\x00' * 16001,
                    'metalink-two.bin': b'metalink-two\xff' * 17003}
        entries = []
        for name, payload in expected.items():
            self.fixture.documents['/' + name] = payload
            entries.append(f'<file name="{name}"><size>{len(payload)}</size>'
                           f'<hash type="sha-256">{hashlib.sha256(payload).hexdigest()}</hash>'
                           f'<url>{xml.sax.saxutils.escape(self.fixture.base + "/" + name)}</url></file>')
        self.fixture.documents['/files.meta4'] = ('<?xml version="1.0"?>'
            '<metalink xmlns="urn:ietf:params:xml:ns:metalink">' + ''.join(entries) + '</metalink>').encode()
        task = self.create(self.fixture.base + '/files.meta4', 'metalink-output')
        self.start(task)
        self.verify(task, expected)
        print('PASS remote Metalink hashes and payloads', flush=True)
        local = self.root / 'local-metalink.meta4'
        local.write_bytes(self.fixture.documents['/files.meta4'])
        task = self.create(local.as_uri())
        self.start(task)
        self.verify(task, expected)
        print('PASS local Metalink metadata and HTTP payloads', flush=True)

        for extension, content in (('torrent', b'd4:infothis-is-not-bencode'),
                                   ('meta4', b'<metalink><file broken')):
            self.fixture.documents['/invalid.' + extension] = content
            task = self.create(self.fixture.base + '/invalid.' + extension, 'invalid-' + extension)
            self.start(task)
            current = self.state(task, ('failed',))
            assert not (self.downloads / current['file_name']).exists(), current
            self.delete(task)
        print('PASS malformed metadata fails without publishing descriptor', flush=True)

        task, torrent = self.blocked('pause')
        self.api('/' + task['id'] + '/pause', {})
        self.state(task, ('paused',), 10)
        stage = self.stage(task)
        assert stage.is_dir(), f'pause discarded stage: {task}'
        def snapshot():
            return {path.relative_to(stage).as_posix(): hashlib.sha1(path.read_bytes()).hexdigest()
                    for path in stage.rglob('*') if path.is_file()}
        before = snapshot()
        assert before, f'pause retained no files: {task}'
        time.sleep(.2)
        assert stage.is_dir() and snapshot() == before, f'paused stage changed: {task}'
        torrent.release.set()
        self.start(task)
        final = self.verify(task, torrent.expected)
        self.delete(task)
        assert final.is_dir(), 'deleting completed task removed committed payload'
        print('PASS pause retains stage, resume commits, completed delete preserves payload', flush=True)

        task, torrent = self.blocked('delete-active')
        self.delete(task)
        torrent.release.set()
        time.sleep(.2)
        assert not self.stage(task).exists(), 'active worker recreated deleted stage'
        assert not (self.downloads / task['file_name']).exists(), task
        print('PASS active deletion removes stage', flush=True)

        first, first_torrent = self.blocked('concurrent-first')
        second_torrent, second_url = self.fixture.torrent('concurrent-second', blocked=True)
        second = self.create(second_url, 'concurrent-second-output')
        self.start(second)
        # With the serialized aria engine this task waits for the first task;
        # a parallel engine may already have requested its first payload block.
        self.state(second, ('queued', 'downloading'))
        self.delete(second)
        second_torrent.release.set()
        first_torrent.release.set()
        self.verify(first, first_torrent.expected)
        assert not self.stage(second).exists(), 'cancelled waiting task later created a stage'
        assert not (self.downloads / second['file_name']).exists(), second
        print('PASS concurrent tasks and cancellation while engine is occupied', flush=True)

    def close(self):
        if self.daemon.poll() is None:
            self.daemon.terminate()
            try:
                self.daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.daemon.kill()
                self.daemon.wait(timeout=5)
        self.log.close()


def main():
    if len(sys.argv) != 2 or not pathlib.Path(sys.argv[1]).is_absolute():
        raise SystemExit('usage: python3 tests/test-bittorrent.py /absolute/path/downloader')
    with tempfile.TemporaryDirectory(prefix='downloader-bt-') as directory:
        fixture = Fixture()
        runner = None
        try:
            runner = Integration(sys.argv[1], pathlib.Path(directory), fixture)
            runner.run()
        except Exception:
            if runner is not None:
                runner.log.flush()
                print('\n--- downloader log (tail) ---', file=sys.stderr)
                print((pathlib.Path(directory) / 'daemon.log').read_text(errors='replace')[-20000:],
                      file=sys.stderr)
            raise
        finally:
            if runner is not None:
                runner.close()
            fixture.close()


if __name__ == '__main__':
    main()
