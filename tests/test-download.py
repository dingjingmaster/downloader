import http.server
import json
import pathlib
import subprocess
import tempfile
import threading
import time
import urllib.request
import sys
import os
import sqlite3

payload = b'downloader-local-fixture\n' * 8192

class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/ua-required' and not self.headers.get('User-Agent', '').startswith('downloader/'):
            self.send_error(403, 'denied by UA ACL = blacklist')
            return
        if self.path == '/forbidden':
            self.send_error(403, 'fixture forbidden')
            return
        offset = int(self.headers.get('Range', 'bytes=0-').split('=')[1].split('-')[0])
        self.send_response(206 if offset else 200)
        if self.path == '/attachment':
            self.send_header('Content-Disposition', "attachment; filename*=UTF-8''report%20final.bin")
        if offset:
            self.send_header('Content-Range', f'bytes {offset}-{len(payload)-1}/{len(payload)}')
        self.send_header('Content-Length', str(len(payload) - offset))
        self.end_headers()
        try:
            for index in range(offset, len(payload), 4096):
                self.wfile.write(payload[index:index + 4096])
                self.wfile.flush()
                if self.path == '/interrupt':
                    return
                if self.path == '/slow':
                    time.sleep(.02)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass

with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    import socket
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    config = root / 'download.ini'
    config.write_text(f'[daemon]\nipc_socket={root}/ipc.sock\n[download]\ndatabase={root}/meta.db\ndownload_dir={root}\n[web]\nlisten=127.0.0.1\nport={port}\n')
    environment = dict(os.environ, no_proxy='127.0.0.1', NO_PROXY='127.0.0.1')
    daemon = subprocess.Popen([sys.argv[1], '--config', str(config)], env=environment)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def api(path, body=None, method=None):
        request = urllib.request.Request(f'http://127.0.0.1:{port}/api/tasks' + path,
            data=None if body is None else json.dumps(body).encode(),
            headers={'Content-Type': 'application/json'}, method=method)
        with opener.open(request, timeout=3) as response:
            return json.load(response)
    try:
        for _ in range(100):
            try:
                api('')
                break
            except OSError:
                time.sleep(.02)
        task = api('', {'url': f'http://127.0.0.1:{server.server_port}/file',
            'file_name': 'result.bin', 'target_directory': directory, 'protocol': 'http'})['task']
        api('/' + task['id'] + '/start', {})
        for _ in range(200):
            current = api('/' + task['id'])['task']
            if current['state'] in ('completed', 'failed'):
                break
            time.sleep(.02)
        assert current['state'] == 'completed', current
        assert (root / 'result.bin').read_bytes() == payload
        assert not (root / f'.{task["id"]}.part').exists()
        assert current['downloaded_bytes'] == len(payload), current
        print('actual HTTP download and atomic commit: PASS')
        def create(name, endpoint):
            return api('', {'url': f'http://127.0.0.1:{server.server_port}/{endpoint}',
                'file_name': name, 'target_directory': directory, 'protocol': 'http'})['task']['id']
        def wait_state(identifier, states):
            for _ in range(300):
                current = api('/' + identifier)['task']
                if current['state'] in states:
                    return current
                time.sleep(.02)
            raise AssertionError(current)
        identifier = create('ua-required.bin', 'ua-required')
        api('/' + identifier + '/start', {})
        current = wait_state(identifier, ['completed', 'failed'])
        assert current['state'] == 'completed', current
        assert (root / 'ua-required.bin').read_bytes() == payload
        identifier = create('forbidden.bin', 'forbidden')
        api('/' + identifier + '/start', {})
        current = wait_state(identifier, ['completed', 'failed'])
        assert current['state'] == 'failed' and 'HTTP 403' in current['error_message'], current
        print('download User-Agent and HTTP error status: PASS')
        identifier = create('resume.bin', 'file')
        (root / f'.{identifier}.part').write_bytes(payload[:8192])
        api('/' + identifier + '/start', {})
        assert wait_state(identifier, ['completed', 'failed'])['state'] == 'completed'
        assert (root / 'resume.bin').read_bytes() == payload
        identifier = create('pause.bin', 'slow')
        api('/' + identifier + '/start', {})
        wait_state(identifier, ['downloading'])
        time.sleep(.1)
        api('/' + identifier + '/pause', {})
        size = (root / f'.{identifier}.part').stat().st_size
        time.sleep(.1)
        assert (root / f'.{identifier}.part').stat().st_size == size
        api('/' + identifier + '/start', {})
        assert wait_state(identifier, ['completed', 'failed'])['state'] == 'completed'
        assert (root / 'pause.bin').read_bytes() == payload
        api('/' + identifier, method='DELETE')
        assert (root / 'pause.bin').read_bytes() == payload
        identifier = create('delete.bin', 'slow')
        api('/' + identifier + '/start', {})
        wait_state(identifier, ['downloading'])
        api('/' + identifier, method='DELETE')
        assert not (root / f'.{identifier}.part').exists()
        assert not (root / 'delete.bin').exists()
        identifier = create('failed.bin', 'interrupt')
        api('/' + identifier + '/start', {})
        assert wait_state(identifier, ['completed', 'failed'])['state'] == 'failed'
        assert (root / f'.{identifier}.part').exists()
        assert not (root / 'failed.bin').exists()
        print('range resume, pause/resume, active delete, interrupted transfer: PASS')
        task = api('', {'url': f'http://127.0.0.1:{server.server_port}/attachment'})['task']
        assert task['file_name'] == 'attachment'
        assert task['protocol'] == 'http'
        assert task['target_directory'] == directory
        api('/' + task['id'] + '/start', {})
        current = wait_state(task['id'], ['completed', 'failed'])
        assert current['state'] == 'completed', current
        assert current['file_name'] == 'report final.bin', current
        assert (root / 'report final.bin').read_bytes() == payload
        print('URL-only creation, configured directory, Content-Disposition: PASS')
        for hardlink in (False, True):
            identifier = create(f'linked-{hardlink}.bin', 'file')
            sentinel = root / f'sentinel-{hardlink}'
            sentinel.write_bytes(b'keep-existing-file')
            partial = root / f'.{identifier}.part'
            if hardlink:
                os.link(sentinel, partial)
            else:
                partial.symlink_to(sentinel)
            api('/' + identifier + '/start', {})
            assert wait_state(identifier, ['completed', 'failed'])['state'] == 'failed'
            assert sentinel.read_bytes() == b'keep-existing-file'
            api('/' + identifier, method='DELETE')
            assert sentinel.read_bytes() == b'keep-existing-file'
        print('partial symlink/hardlink targets remain untouched: PASS')
        identifier = create('shutdown.bin', 'slow')
        api('/' + identifier + '/start', {})
        wait_state(identifier, ['downloading'])
        time.sleep(.1)
        daemon.terminate()
        assert daemon.wait(timeout=5) == 0
        with sqlite3.connect(root / 'meta.db') as database:
            assert database.execute('SELECT state FROM download_tasks WHERE uuid=?', (identifier,)).fetchone() == ('paused',)
        daemon = subprocess.Popen([sys.argv[1], '--config', str(config)], env=environment)
        for _ in range(100):
            try:
                api('')
                break
            except OSError:
                time.sleep(.02)
        api('/' + identifier + '/start', {})
        assert wait_state(identifier, ['completed', 'failed'])['state'] == 'completed'
        assert (root / 'shutdown.bin').read_bytes() == payload
        print('graceful daemon shutdown and persisted resume: PASS')
    finally:
        daemon.terminate()
        daemon.wait(timeout=5)
        server.shutdown()
