#!/usr/bin/env python3
"""Loopback tests for the three-connection HTTP downloader.

Usage: test-http-parallel.py /absolute/path/downloader
"""
import http.server
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

PAYLOAD = bytes(range(256)) * 16384          # 4 MiB
SMALL = bytes(range(256)) * 1024             # 256 KiB
BINARY = None


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def record(self, method):
        entry = {'method': method, 'range': self.headers.get('Range'), 'start': time.monotonic(), 'end': None}
        with self.server.lock:
            self.server.requests.append(entry)
        return entry

    def content(self):
        return self.server.documents.get(self.path.split('?', 1)[0])

    def do_HEAD(self):
        data = self.content()
        if data is None:
            self.send_error(404)
            return
        self.record('HEAD')
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('ETag', '"parallel-v1"')
        self.send_header('Last-Modified', 'Wed, 21 Oct 2015 07:28:00 GMT')
        self.send_header('Accept-Ranges', 'bytes')
        self.end_headers()

    def do_GET(self):
        data = self.content()
        if data is None:
            self.send_error(404)
            return
        entry = self.record('GET')
        ranged = self.headers.get('Range')
        status, body, extra = 200, data, {}
        if ranged and not self.server.ignore_range:
            spec = ranged.removeprefix('bytes=').split('-', 1)
            start = int(spec[0])
            end = int(spec[1]) if spec[1] else len(data) - 1
            end = min(end, len(data) - 1)
            status, body = 206, data[start:end + 1]
            extra['Content-Range'] = f'bytes {start}-{end}/{len(data)}'
            extra['ETag'] = '"parallel-v1"'
        try:
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            for key, value in extra.items():
                self.send_header(key, value)
            self.end_headers()
            for index in range(0, len(body), 65536):
                self.wfile.write(body[index:index + 65536])
                self.wfile.flush()
                if self.server.delay:
                    time.sleep(self.server.delay)
            entry['end'] = time.monotonic()
        except (BrokenPipeError, ConnectionResetError):
            entry['end'] = time.monotonic()


class Daemon:
    def __init__(self, binary, root):
        self.root = root
        self.downloads = root / 'downloads'
        self.downloads.mkdir()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.config = root / 'daemon.ini'
        self.config.write_text(
            f'[daemon]\nipc_socket={root}/ipc.sock\n'
            f'[download]\ndatabase={root}/meta.db\ndownload_dir={self.downloads}\nmax_retries=0\n'
            f'[web]\nlisten=127.0.0.1\nport={self.port}\n', encoding='utf-8')
        self.log = open(root / 'daemon.log', 'w+b')
        self.environment = dict(os.environ, NO_PROXY='127.0.0.1', no_proxy='127.0.0.1')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.process = subprocess.Popen([binary, '--config', str(self.config)], env=self.environment,
                                        stdout=self.log, stderr=self.log)

    def api(self, suffix='', body=None, method=None):
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/tasks{suffix}',
            data=None if body is None else json.dumps(body).encode(), method=method,
            headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=5) as response:
            raw = response.read()
            return json.loads(raw) if raw else {}

    def ready(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read().decode(errors='replace'))
            try:
                self.api()
                return
            except (OSError, ValueError):
                time.sleep(.02)
        raise AssertionError('daemon API did not become ready')

    def create(self, url, name):
        return self.api('', {'url': url, 'file_name': name, 'target_directory': str(self.downloads)})['task']

    def wait(self, task, states=('completed', 'failed'), timeout=30):
        deadline = time.monotonic() + timeout
        current = None
        while time.monotonic() < deadline:
            current = self.api('/' + task['id'])['task']
            if current['state'] in states:
                return current
            time.sleep(.01)
        raise AssertionError(f'task deadline exceeded: {current}')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()


def make_server(documents, ignore_range=False, delay=0):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    server.daemon_threads = True
    server.documents = documents
    server.ignore_range = ignore_range
    server.delay = delay
    server.requests = []
    server.lock = threading.Lock()
    threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .02}, daemon=True).start()
    return server


def main():
    global BINARY
    if len(sys.argv) != 2 or not pathlib.Path(sys.argv[1]).is_absolute():
        raise SystemExit('usage: test-http-parallel.py /absolute/path/downloader')
    BINARY = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='http-parallel-') as directory:
        root = pathlib.Path(directory)
        daemon = Daemon(BINARY, root)
        server = make_server({'/big.bin': PAYLOAD}, delay=.02)
        small = make_server({'/small.bin': SMALL})
        ignored = make_server({'/ignored.bin': PAYLOAD}, ignore_range=True, delay=.002)
        try:
            daemon.ready()
            base = f'http://127.0.0.1:{server.server_port}'
            task = daemon.create(base + '/big.bin', 'big-1.bin')
            daemon.api('/' + task['id'] + '/start', {})
            current = daemon.wait(task)
            assert current['state'] == 'completed', current
            assert (daemon.downloads / 'big-1.bin').read_bytes() == PAYLOAD
            requests = [entry for entry in server.requests if entry['method'] == 'GET']
            assert len(requests) == 3, server.requests
            assert all(entry['range'] for entry in requests), requests
            assert max(entry['start'] for entry in requests) < min(entry['end'] for entry in requests), requests
            print('PASS three concurrent range connections', flush=True)

            # Pause after some bytes, then resume from the per-segment files.
            task = daemon.create(base + '/big.bin', 'big-2.bin')
            daemon.api('/' + task['id'] + '/start', {})
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if daemon.api('/' + task['id'])['task']['downloaded_bytes'] > 0:
                    break
                time.sleep(.01)
            daemon.api('/' + task['id'] + '/pause', {})
            daemon.wait(task, ('paused',), timeout=5)
            partial = daemon.downloads / f'.{task["id"]}.part'
            segments = pathlib.Path(str(partial) + '.segments')
            assert segments.is_dir(), 'parallel segment directory missing'
            before = {index: (segments / f'seg{index}').stat().st_size
                      for index in range(3) if (segments / f'seg{index}').exists()}
            with server.lock:
                server.requests.clear()
            daemon.api('/' + task['id'] + '/start', {})
            current = daemon.wait(task)
            assert current['state'] == 'completed', current
            assert (daemon.downloads / 'big-2.bin').read_bytes() == PAYLOAD
            resumed = [entry for entry in server.requests if entry['method'] == 'GET']
            assert 1 <= len(resumed) <= 3, resumed
            assert all(entry['range'] for entry in resumed), resumed
            assert any(int(entry['range'].removeprefix('bytes=').split('-')[0]) > 0 for entry in resumed), resumed
            print('PASS parallel pause/resume reuses segment prefixes', flush=True)

            # A HEAD-capable but small resource uses the single stream.
            task = daemon.create(f'http://127.0.0.1:{small.server_port}/small.bin', 'small.bin')
            daemon.api('/' + task['id'] + '/start', {})
            current = daemon.wait(task)
            assert current['state'] == 'completed', current
            assert (daemon.downloads / 'small.bin').read_bytes() == SMALL
            gets = [entry for entry in small.requests if entry['method'] == 'GET']
            assert len(gets) == 1 and not gets[0]['range'], small.requests
            print('PASS small resource falls back to a single stream', flush=True)

            # A server that ignores Range falls back to a full single stream.
            task = daemon.create(f'http://127.0.0.1:{ignored.server_port}/ignored.bin', 'ignored.bin')
            daemon.api('/' + task['id'] + '/start', {})
            current = daemon.wait(task)
            assert current['state'] == 'completed', current
            assert (daemon.downloads / 'ignored.bin').read_bytes() == PAYLOAD
            print('PASS ignored Range falls back to the single stream', flush=True)
        except BaseException:
            print((root / 'daemon.log').read_text(errors='replace')[-3000:], file=sys.stderr)
            raise
        finally:
            daemon.close()
            for running in (server, small, ignored):
                running.shutdown()
                running.server_close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
