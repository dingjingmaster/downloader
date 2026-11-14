#!/usr/bin/env python3
"""Loopback-only Baidu protocol integration tests.

Usage: python3 -B tests/test-baidu.py /absolute/path/downloader
The daemon is pointed at a local mock through [baidu] api_base/pcs_base.
"""
import hashlib
import http.server
import json
import os
import pathlib
import posixpath
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

PAYLOAD = b'baidu-fixture-payload\n' * 8192
BINARY = None


class MockState:
    def __init__(self, files, share=None):
        self.files = []
        for index, (path, payload) in enumerate(files.items()):
            if not path.startswith('/'):
                path = '/' + path
            self.files.append((index + 1, path, payload))
        self.directories = set()
        for _, path, _ in self.files:
            parent = posixpath.dirname(path)
            while parent and parent != '/':
                self.directories.add(parent)
                parent = posixpath.dirname(parent)
        self.directory_ids = {}
        next_id = len(self.files) + 1
        for directory in sorted(self.directories):
            self.directory_ids[directory] = next_id
            next_id += 1
        self.next_fs_id = next_id
        self.share = share
        self.share_files = []
        self.share_directories = set()
        self.share_ids = {}
        self.verified = False
        self.transfers = []
        if share:
            for index, (path, payload) in enumerate(share.get('files', {}).items()):
                if not path.startswith('/'):
                    path = '/' + path
                self.share_files.append((index + 1, path, payload))
            for _, path, _ in self.share_files:
                parent = posixpath.dirname(path)
                while parent and parent != '/':
                    self.share_directories.add(parent)
                    parent = posixpath.dirname(parent)
            next_id = len(self.share_files) + 1
            for directory in sorted(self.share_directories):
                self.share_ids[directory] = next_id
                next_id += 1
        self.disk_home_calls = 0
        self.download_calls = 0
        self.dlink_calls = []
        self.first_download_errno = None
        self.dlink_forbidden_once = False
        self.interrupt_once = False
        self.delay = 0.0
        self.dlink_hold = 0.0
        self.dlink_error_once = False
        self.disk_home_error_once = False
        self.transfer_duplicate_once = False
        self.transfer_calls = 0
        self.omit_md5 = False
        self.active = 0
        self.max_active = 0
        self.lock = threading.Lock()

    def file_entry(self, fs_id, path, payload):
        return {
            'fs_id': fs_id,
            'isdir': 0,
            'server_filename': posixpath.basename(path),
            'size': len(payload),
            'md5': '' if self.omit_md5 else hashlib.md5(payload).hexdigest(),
            'path': path,
            'server_mtime': 1690000000,
        }

    def directory_entry(self, path):
        return {
            'fs_id': self.directory_ids[path],
            'isdir': 1,
            'server_filename': posixpath.basename(path),
            'size': 0,
            'md5': '',
            'path': path,
            'server_mtime': 1690000000,
        }

    def entries(self, directory):
        directory = directory or '/'
        if not directory.startswith('/'):
            directory = '/' + directory
        entries = [self.file_entry(fs_id, path, payload) for fs_id, path, payload in self.files
                   if posixpath.dirname(path) == directory]
        entries.extend(self.directory_entry(path) for path in sorted(self.directories)
                       if posixpath.dirname(path) == directory)
        return entries

    def payload_for(self, fs_id):
        return next((payload for current, _, payload in self.files if current == fs_id), None)

    def share_entry_by_fsid(self, fs_id):
        for current, path, payload in self.share_files:
            if current == fs_id:
                return self.file_entry(current, path, payload), payload
        for path, current in self.share_ids.items():
            if current == fs_id:
                return {
                    'fs_id': current,
                    'isdir': 1,
                    'server_filename': posixpath.basename(path),
                    'size': 0,
                    'md5': '',
                    'path': path,
                    'server_mtime': 1690000000,
                }, None
        return None, None

    def share_root_entries(self):
        entries = [self.file_entry(fs_id, path, payload) for fs_id, path, payload in self.share_files
                   if posixpath.dirname(path) == '/']
        entries.extend({
            'fs_id': self.share_ids[path],
            'isdir': 1,
            'server_filename': posixpath.basename(path),
            'size': 0,
            'md5': '',
            'path': path,
            'server_mtime': 1690000000,
        } for path in sorted(self.share_directories) if posixpath.dirname(path) == '/')
        return entries

    def add_local_file(self, path, payload):
        self.files.append((self.next_fs_id, path, payload))
        self.next_fs_id += 1
        parent = posixpath.dirname(path)
        while parent and parent != '/':
            if parent not in self.directories:
                self.directories.add(parent)
                self.directory_ids[parent] = self.next_fs_id
                self.next_fs_id += 1
            parent = posixpath.dirname(parent)

    def transfer_entry(self, fs_id, transfer_dir):
        entry, payload = self.share_entry_by_fsid(fs_id)
        if entry is None:
            return None
        if entry['isdir']:
            root = entry['path']
            for _, path, file_payload in self.share_files:
                if path.startswith(root + '/'):
                    self.add_local_file(transfer_dir + path, file_payload)
            return transfer_dir + root
        self.add_local_file(transfer_dir + entry['path'], payload)
        return transfer_dir + entry['path']


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=None):
        self.send_response(status)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        state = self.server.state
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == '/disk/home':
            state.disk_home_calls += 1
            if state.disk_home_error_once:
                state.disk_home_error_once = False
                self.reply(500, b'disk-home-error-body')
                return
            html = ('<html><script>locals.mset({'
                    '"sign1":"37dbe07ade9359c1aa70807e847f768c13360ad2",'
                    '"sign3":"e8c7d729eea7b54551aa594f942decbe",'
                    '"timestamp":1690000000,});</script></html>').encode()
            self.reply(200, html, {'Content-Type': 'text/html'})
            return
        if parsed.path.startswith('/s/'):
            mode = state.share.get('mode', 'ok') if state.share else 'not_found'
            if mode == 'expired':
                self.reply(200, b'<html>platform-non-found</html>', {'Content-Type': 'text/html'})
            elif mode == 'not_found':
                self.reply(200, b'<html>error-404</html>', {'Content-Type': 'text/html'})
            else:
                html = ('<html><script>locals.mset({'
                        '"loginstate":1,"bdstoken":"tok","uk":"111","share_uk":"222",'
                        '"shareid":"333","title":"share-title"});</script></html>').encode()
                self.reply(200, html, {'Content-Type': 'text/html'})
            return
        if parsed.path == '/share/list':
            if state.share is None:
                self.reply(404, b'')
                return
            if state.share.get('pwd') and not state.verified:
                self.reply(200, json.dumps({'errno': -9}).encode())
                return
            self.reply(200, json.dumps({'errno': 0, 'list': state.share_root_entries()}).encode())
            return
        if parsed.path == '/api/list':
            query = urllib.parse.parse_qs(parsed.query)
            directory = query.get('dir', ['/'])[0]
            self.reply(200, json.dumps({'errno': 0, 'list': state.entries(directory)}).encode())
            return
        if parsed.path.startswith('/dlink/'):
            self.serve_dlink(parsed.path.rsplit('/', 1)[1])
            return
        self.reply(404, b'')

    def serve_dlink(self, fsid):
        state = self.server.state
        fsid = int(fsid)
        payload = state.payload_for(fsid)
        if payload is None:
            self.reply(404, b'')
            return
        with state.lock:
            state.dlink_calls.append(fsid)
            state.active += 1
            state.max_active = max(state.max_active, state.active)
        try:
            if state.dlink_hold:
                time.sleep(state.dlink_hold)
            if state.dlink_error_once:
                state.dlink_error_once = False
                self.reply(500, b'server-error-body')
                return
            if state.dlink_forbidden_once:
                state.dlink_forbidden_once = False
                self.reply(403, b'forbidden-body')
                return
            offset = 0
            range_header = self.headers.get('Range', '')
            if range_header.startswith('bytes='):
                offset = int(range_header[6:].split('-', 1)[0] or 0)
            body = payload[offset:]
            headers = {'Content-Type': 'application/octet-stream'}
            if offset:
                headers['Content-Range'] = f'bytes {offset}-{len(payload) - 1}/{len(payload)}'
            self.send_response(206 if offset else 200)
            for key, value in headers.items():
                self.send_header(key, value)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            if state.interrupt_once:
                state.interrupt_once = False
                self.wfile.write(body[:len(body) // 2])
                self.wfile.flush()
                self.close_connection = True
                return
            for index in range(0, len(body), 4096):
                self.wfile.write(body[index:index + 4096])
                self.wfile.flush()
                if state.delay:
                    time.sleep(state.delay)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with state.lock:
                state.active -= 1

    def do_POST(self):
        state = self.server.state
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length).decode()
        parsed = urllib.parse.urlparse(self.path)
        form = urllib.parse.parse_qs(body)
        query = urllib.parse.parse_qs(parsed.query)
        if parsed.path == '/share/verify':
            pwd = form.get('pwd', [''])[0]
            if state.share is not None and pwd == state.share.get('pwd'):
                state.verified = True
                self.reply(200, json.dumps({'errno': 0, 'randsk': 'r'}).encode(),
                           {'Set-Cookie': 'BDCLND=ok; Path=/'})
            else:
                self.reply(200, json.dumps({'errno': -9}).encode())
            return
        if parsed.path == '/share/transfer':
            state.transfer_calls += 1
            value = query.get('fs_id', [''])[0] or form.get('fsidlist', [''])[0]
            ids = [int(digits) for digits in re.findall(r'\d+', value)]
            transfer_dir = '/downloader/' + (state.share or {}).get('surl', 'share')
            transferred = []
            for fsid in ids:
                path = state.transfer_entry(fsid, transfer_dir)
                if path is not None:
                    state.transfers.append((fsid, path))
                    transferred.append({'path': path})
            if not transferred:
                self.reply(200, json.dumps({'errno': 12, 'info': [{'errno': -9, 'path': ''}]}).encode())
                return
            if state.transfer_duplicate_once:
                state.transfer_duplicate_once = False
                self.reply(200, json.dumps({'errno': 12,
                                            'info': [{'errno': -30, 'path': entry['path']}
                                                     for entry in transferred]}).encode())
                return
            self.reply(200, json.dumps({'errno': 0, 'info': transferred}).encode())
            return
        if parsed.path == '/rest/2.0/pcs/file':
            if query.get('method', [''])[0] == 'mkdir':
                self.reply(200, json.dumps({'errno': 0}).encode())
            else:
                self.reply(404, b'')
            return
        if self.path.startswith('/api/download'):
            state.download_calls += 1
            if state.first_download_errno is not None:
                errno = state.first_download_errno
                state.first_download_errno = None
                self.reply(200, json.dumps({'errno': errno}).encode())
                return
            form = urllib.parse.parse_qs(body)
            value = form.get('fidlist', [''])[0]
            digits = ''.join(character for character in value if character.isdigit())
            fsid = int(digits) if digits else 1
            dlink = f'http://127.0.0.1:{self.server.server_port}/dlink/{fsid}'
            self.reply(200, json.dumps({'errno': 0, 'dlink': [{'fs_id': str(fsid), 'dlink': dlink}]}).encode())
            return
        self.reply(404, b'')


class Harness:
    def __init__(self, files, max_parallel=2, share=None, max_files=2000):
        self.directory = tempfile.TemporaryDirectory(prefix='baidu-test-')
        self.root = pathlib.Path(self.directory.name)
        self.downloads = self.root / 'downloads'
        self.downloads.mkdir()
        self.state = MockState(files, share)
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.state = self.state
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01}, daemon=True).start()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.config = self.root / 'daemon.ini'
        self.config.write_text(
            f'[daemon]\nipc_socket={self.root}/ipc.sock\n'
            f'[download]\ndatabase={self.root}/meta.db\ndownload_dir={self.downloads}\nmax_retries=0\n'
            f'[web]\nlisten=127.0.0.1\nport={self.port}\n'
            f'[baidu]\napi_base=http://127.0.0.1:{self.server.server_port}\n'
            f'pcs_base=http://127.0.0.1:{self.server.server_port}\n'
            f'max_parallel_tasks={max_parallel}\nmax_files={max_files}\nsign_cache_seconds=3600\n'
            f'transfer_directory=/downloader\n', encoding='utf-8')
        self.log = open(self.root / 'daemon.log', 'w+b')
        environment = {key: value for key, value in os.environ.items()
                       if key.lower() not in ('http_proxy', 'https_proxy', 'all_proxy')}
        environment.update(no_proxy='127.0.0.1', NO_PROXY='127.0.0.1')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.process = subprocess.Popen([BINARY, '--config', str(self.config)], env=environment,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read().decode(errors='replace'))
            try:
                self.api('')
                return
            except (OSError, ValueError):
                time.sleep(.02)
        raise AssertionError('daemon API did not become ready')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()
        self.server.shutdown()
        self.server.server_close()
        self.directory.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if exc_type is not None:
            print(f'daemon exit status: {self.process.poll()}', file=sys.stderr)
            self.log.seek(0)
            print(self.log.read().decode(errors='replace'), file=sys.stderr)
        self.close()
        return False

    def api(self, suffix, body=None, method=None):
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/tasks{suffix}',
            data=None if body is None else json.dumps(body).encode(), method=method,
            headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=5) as response:
            raw = response.read()
            return json.loads(raw) if raw else {}

    def profile(self):
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/auth-profiles',
            data=json.dumps({'name': 'baidu-acct', 'kind': 'cookie',
                             'secret': 'BDUSS=session; STOKEN=netdisk'}).encode(),
            method='POST', headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=5) as response:
            return json.loads(response.read())

    def create(self, url, extra=None):
        status, response = self.create_response(url, extra)
        assert status in (200, 201), (status, response)
        if response.get('task'):
            return response['task']
        assert response.get('tasks'), response
        return response['tasks'][0]

    def create_response(self, url, extra=None):
        body = {'url': url, 'target_directory': str(self.downloads)}
        if extra is not None:
            body['extra'] = extra
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/tasks', data=json.dumps(body).encode(),
            method='POST', headers={'Content-Type': 'application/json'})
        try:
            with self.opener.open(request, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def start(self, task):
        self.api('/' + task['id'] + '/start', {})

    def pause(self, task):
        self.api('/' + task['id'] + '/pause', {})

    def resume(self, task):
        self.api('/' + task['id'] + '/resume', {})

    def group_action(self, group_id, action=None, method='POST'):
        suffix = f'/{action}' if action else ''
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/groups/{group_id}{suffix}',
            data=b'{}' if method == 'POST' else None, method=method,
            headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=10) as response:
            return json.loads(response.read())

    def ipc(self, body):
        payload = json.dumps(body).encode()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(str(self.root / 'ipc.sock'))
            client.sendall(struct.pack('<6I', 0x43495043, 1, 1, 1, len(payload), 0) + payload)
            header = b''
            while len(header) < 24:
                chunk = client.recv(24 - len(header))
                assert chunk
                header += chunk
            magic, version, _, _, length, _ = struct.unpack('<6I', header)
            assert magic == 0x43495043 and version == 1
            data = b''
            while len(data) < length:
                chunk = client.recv(length - len(data))
                assert chunk
                data += chunk
            return json.loads(data)

    def wait(self, task, timeout=20):
        deadline = time.monotonic() + timeout
        current = None
        while time.monotonic() < deadline:
            current = self.api('/' + task['id'])['task']
            if current['state'] in ('completed', 'failed'):
                return current
            time.sleep(.02)
        raise AssertionError(f'task deadline exceeded: {current}')

    def finish(self, task):
        current = self.wait(task)
        assert current['state'] == 'completed', current
        self.api('/' + task['id'], method='DELETE')
        return current


def scenario_own_single_file():
    with Harness({'a.bin': PAYLOAD}) as harness:
        harness.profile()
        task = harness.create('baidu:///a.bin', {'auth_profile': 'baidu-acct'})
        harness.start(task)
        harness.finish(task)
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        assert harness.state.download_calls == 1
        print('PASS baidu own single file', flush=True)


def scenario_resume():
    with Harness({'a.bin': PAYLOAD}) as harness:
        harness.state.interrupt_once = True
        harness.profile()
        task = harness.create('baidu:///a.bin', {'auth_profile': 'baidu-acct'})
        harness.start(task)
        harness.finish(task)
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        assert len(harness.state.dlink_calls) >= 2
        print('PASS baidu resume after a broken dlink stream', flush=True)


def scenario_dlink_forbidden():
    with Harness({'a.bin': PAYLOAD}) as harness:
        harness.state.dlink_forbidden_once = True
        harness.profile()
        task = harness.create('baidu:///a.bin', {'auth_profile': 'baidu-acct'})
        harness.start(task)
        harness.finish(task)
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        assert harness.state.download_calls >= 2
        print('PASS baidu re-resolves a forbidden dlink', flush=True)


def scenario_auth_missing():
    with Harness({'a.bin': PAYLOAD}) as harness:
        status, response = harness.create_response('baidu:///a.bin')
        assert status >= 400, (status, response)
        assert response['error'] == 'baidu_auth_missing', response
        assert harness.state.disk_home_calls == 0
        assert harness.state.download_calls == 0
        print('PASS baidu refuses to create tasks without credentials', flush=True)


def scenario_own_directory():
    second = b'directory-second-payload\n' * 32768
    with Harness({'/资料/a.bin': PAYLOAD, '/资料/2024/b.bin': second}) as harness:
        harness.state.delay = .01
        harness.profile()
        status, response = harness.create_response('baidu:///%E8%B5%84%E6%96%99',
                                                   {'auth_profile': 'baidu-acct'})
        assert status == 201, response
        assert response['group']['task_count'] == 2, response
        tasks = harness.api('?page=1&page_size=50')['tasks']
        assert len(tasks) == 2, tasks
        harness.start(tasks[0])
        harness.start(tasks[1])
        harness.pause(tasks[1])
        current = harness.api('/' + tasks[1]['id'])['task']
        assert current['state'] == 'paused', current
        harness.resume(tasks[1])
        for task in tasks:
            harness.finish(task)
        assert (harness.downloads / '资料' / 'a.bin').read_bytes() == PAYLOAD
        assert (harness.downloads / '资料' / '2024' / 'b.bin').read_bytes() == second
        print('PASS baidu own directory expansion and pause/resume', flush=True)


def scenario_target_exists():
    with Harness({'/资料/a.bin': PAYLOAD, '/资料/2024/b.bin': PAYLOAD}) as harness:
        harness.profile()
        (harness.downloads / '资料').mkdir(parents=True)
        (harness.downloads / '资料' / 'a.bin').write_bytes(b'existing')
        status, response = harness.create_response('baidu:///%E8%B5%84%E6%96%99',
                                                   {'auth_profile': 'baidu-acct'})
        assert status == 201, response
        assert response['group']['skipped'] == 1, response
        assert response['group']['task_count'] == 1, response
        print('PASS baidu skips existing local targets', flush=True)


def scenario_sign_refresh():
    with Harness({'a.bin': PAYLOAD}) as harness:
        harness.state.first_download_errno = 113
        harness.profile()
        task = harness.create('baidu:///a.bin', {'auth_profile': 'baidu-acct'})
        harness.start(task)
        harness.finish(task)
        assert harness.state.disk_home_calls == 2, harness.state.disk_home_calls
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        print('PASS baidu refreshes an expired sign', flush=True)


def scenario_parallel_limit():
    payload = b'parallel-fixture\n' * 16384
    with Harness({'r1.bin': payload, 'r2.bin': payload}, max_parallel=1) as harness:
        harness.state.dlink_hold = .8
        harness.profile()
        first = harness.create('baidu:///r1.bin', {'auth_profile': 'baidu-acct'})
        second = harness.create('baidu:///r2.bin', {'auth_profile': 'baidu-acct'})
        harness.start(first)
        harness.start(second)
        time.sleep(.2)
        assert len(harness.state.dlink_calls) == 1, harness.state.dlink_calls
        harness.finish(first)
        harness.finish(second)
        print('PASS baidu parallel limit is enforced', flush=True)


def scenario_share_single_file():
    share = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': {'/a.bin': PAYLOAD}}
    with Harness({}, share=share) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 201, response
        assert response['group'], response
        assert harness.state.transfers, harness.state
        harness.start(response['task'])
        harness.finish(response['task'])
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        print('PASS baidu share single-file transfer and download', flush=True)


def scenario_share_password_errors():
    share = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': {'/a.bin': PAYLOAD}}
    with Harness({}, share=share) as harness:
        harness.profile()
        status, response = harness.create_response('https://pan.baidu.com/s/1AbCdEf',
                                                   {'auth_profile': 'baidu-acct'})
        assert status == 400 and response['error'] == 'baidu_password_required', (status, response)
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'wrong'}})
        assert status == 400 and response['error'] == 'baidu_password_invalid', (status, response)
        print('PASS baidu share password errors are explicit', flush=True)


def scenario_share_expired():
    for mode, code in (('expired', 'baidu_share_expired'), ('not_found', 'baidu_share_not_found')):
        share = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': {'/a.bin': PAYLOAD}, 'mode': mode}
        with Harness({}, share=share) as harness:
            harness.profile()
            status, response = harness.create_response(
                'https://pan.baidu.com/s/1AbCdEf',
                {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
            assert status == 404 and response['error'] == code, (status, response)
    print('PASS baidu share expiry errors are explicit', flush=True)


def scenario_share_directory():
    second = b'share-directory-second\n' * 4096
    share = {'surl': '1AbCdEf', 'pwd': 'abcd',
             'files': {'/资料/a.bin': PAYLOAD, '/资料/2024/b.bin': second}}
    with Harness({}, share=share) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 201, response
        assert response['group']['task_count'] == 2, response
        tasks = harness.api('?page=1&page_size=50')['tasks']
        assert len(tasks) == 2, tasks
        for task in tasks:
            harness.start(task)
        for task in tasks:
            harness.finish(task)
        assert (harness.downloads / '资料' / 'a.bin').read_bytes() == PAYLOAD
        assert (harness.downloads / '资料' / '2024' / 'b.bin').read_bytes() == second
        print('PASS baidu share directory expansion', flush=True)


def scenario_share_path_filter():
    second = b'share-filter-second\n' * 4096
    share = {'surl': '1AbCdEf', 'pwd': 'abcd',
             'files': {'/资料/a.bin': PAYLOAD, '/资料/2024/b.bin': second}}
    with Harness({}, share=share) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd', 'path': '/资料/2024'}})
        assert status == 201, response
        assert response['group']['task_count'] == 1, response
        harness.start(response['task'])
        harness.finish(response['task'])
        assert (harness.downloads / '资料' / '2024' / 'b.bin').read_bytes() == second
        assert not (harness.downloads / '资料' / 'a.bin').exists()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd', 'path': '/不存在'}})
        assert status == 404 and response['error'] == 'baidu_share_path_not_found', (status, response)
        print('PASS baidu share path filter', flush=True)


def scenario_share_partial_transfer():
    share = {'surl': '1AbCdEf', 'pwd': 'abcd',
             'files': {'/a.bin': PAYLOAD, '/b.bin': b'second-root-file'}}
    with Harness({'/downloader/1AbCdEf/a.bin': PAYLOAD}, share=share) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 201, response
        assert response['group']['task_count'] == 2, response
        assert len(harness.state.transfers) == 1, harness.state.transfers

    conflict_share = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': {'/a.bin': PAYLOAD}}
    with Harness({'/downloader/1AbCdEf/a.bin': b'different-content'}, share=conflict_share) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 409 and response['error'] == 'baidu_transfer_conflict', (status, response)
        assert not harness.state.transfers
    print('PASS baidu partially transferred shares are reused and conflicts rejected', flush=True)


def scenario_share_limits():
    share = {'surl': '1AbCdEf', 'pwd': 'abcd',
             'files': {'/a.bin': b'a', '/b.bin': b'b', '/c.bin': b'c', '/d.bin': b'd'}}
    with Harness({}, share=share, max_files=3) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 413 and response['error'] == 'baidu_share_too_large', (status, response)

    empty = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': {}}
    with Harness({}, share=empty) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert response['error'] == 'baidu_share_empty', (status, response)
    print('PASS baidu share limits and empty shares', flush=True)


def scenario_group_actions():
    second = b'group-action-second\n' * 4096
    share = {'surl': '1AbCdEf', 'pwd': 'abcd',
             'files': {'/资料/a.bin': PAYLOAD, '/资料/2024/b.bin': second}}
    with Harness({}, share=share) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 201, response
        group_id = response['group']['id']
        harness.group_action(group_id, 'pause')
        tasks = [task for task in harness.api('?page=1&page_size=50')['tasks']
                 if task.get('group_id') == group_id]
        assert len(tasks) == 2, tasks
        assert all(task['state'] == 'paused' for task in tasks), tasks
        harness.group_action(group_id, 'resume')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            tasks = [task for task in harness.api('?page=1&page_size=50')['tasks']
                     if task.get('group_id') == group_id]
            if tasks and all(task['state'] == 'completed' for task in tasks):
                break
            time.sleep(.02)
        assert tasks and all(task['state'] == 'completed' for task in tasks), tasks
        transfers = len(harness.state.transfers)
        harness.group_action(group_id, None, method='DELETE')
        remaining = [task for task in harness.api('?page=1&page_size=50')['tasks']
                     if task.get('group_id') == group_id]
        assert not remaining, remaining
        assert len(harness.state.transfers) == transfers
        print('PASS baidu group actions', flush=True)


def scenario_ipc_create():
    with Harness({'/资料/a.bin': PAYLOAD, '/资料/2024/b.bin': b'ipc-second'}) as harness:
        harness.profile()
        response = harness.ipc({'op': 'create', 'url': 'baidu:///%E8%B5%84%E6%96%99',
                                'extra': {'auth_profile': 'baidu-acct'}})
        assert response['ok'] is True, response
        assert response.get('id'), response
        assert response['group']['task_count'] == 2, response
        assert response.get('skipped', 0) == 0, response
        assert response.get('truncated') is False, response
        response = harness.ipc({'op': 'create', 'url': 'baidu:///%E8%B5%84%E6%96%99/a.bin',
                                'extra': {'auth_profile': 'baidu-acct'}})
        assert response['ok'] is True, response
        print('PASS baidu IPC create expands and stays backward compatible', flush=True)


def scenario_dlink_error_body():
    with Harness({'a.bin': PAYLOAD}) as harness:
        harness.state.omit_md5 = True
        harness.state.dlink_error_once = True
        harness.profile()
        task = harness.create('baidu:///a.bin', {'auth_profile': 'baidu-acct'})
        harness.start(task)
        harness.finish(task)
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        print('PASS baidu discards HTTP error bodies before resuming', flush=True)


def scenario_sign_transient_error():
    with Harness({'a.bin': PAYLOAD}) as harness:
        harness.state.disk_home_error_once = True
        harness.profile()
        task = harness.create('baidu:///a.bin', {'auth_profile': 'baidu-acct'})
        harness.start(task)
        harness.finish(task)
        assert (harness.downloads / 'a.bin').read_bytes() == PAYLOAD
        print('PASS baidu retries a transient /disk/home failure', flush=True)


def scenario_share_duplicate_transfer():
    share = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': {'/a.bin': PAYLOAD}}
    with Harness({}, share=share) as harness:
        harness.state.transfer_duplicate_once = True
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 201, response
        assert response['group']['task_count'] == 1, response
        print('PASS baidu treats transfer errno 12/-30 as already transferred', flush=True)


def scenario_share_chunked_transfer():
    files = {f'/f{index:03d}.bin': b'x' for index in range(120)}
    share = {'surl': '1AbCdEf', 'pwd': 'abcd', 'files': files}
    with Harness({}, share=share, max_files=200) as harness:
        harness.profile()
        status, response = harness.create_response(
            'https://pan.baidu.com/s/1AbCdEf',
            {'auth_profile': 'baidu-acct', 'baidu': {'pwd': 'abcd'}})
        assert status == 201, response
        assert response['group']['task_count'] == 120, response
        assert harness.state.transfer_calls >= 2, harness.state.transfer_calls
        print('PASS baidu chunks large root transfers', flush=True)


def main():
    if len(sys.argv) != 2 or not pathlib.Path(sys.argv[1]).is_absolute():
        raise SystemExit('usage: python3 tests/test-baidu.py /absolute/path/downloader')
    global BINARY
    BINARY = sys.argv[1]
    scenario_own_single_file()
    scenario_resume()
    scenario_dlink_forbidden()
    scenario_auth_missing()
    scenario_sign_refresh()
    scenario_parallel_limit()
    scenario_own_directory()
    scenario_target_exists()
    scenario_share_single_file()
    scenario_share_password_errors()
    scenario_share_expired()
    scenario_share_directory()
    scenario_share_path_filter()
    scenario_share_partial_transfer()
    scenario_share_limits()
    scenario_group_actions()
    scenario_ipc_create()
    scenario_dlink_error_body()
    scenario_sign_transient_error()
    scenario_share_duplicate_transfer()
    scenario_share_chunked_transfer()


if __name__ == '__main__':
    main()
