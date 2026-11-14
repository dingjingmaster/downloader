#!/usr/bin/env python3
"""Loopback-only HTTP auth/proxy/cookie integration.

Usage: python3 tests/test-http-auth.py /absolute/path/downloader
Profiles are created through the local Web API; servers are local fixtures.
"""
import base64
import hashlib
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

PAYLOAD = b'authenticated-fixture-payload\n' * 4096
BINARY = None


def md5(value):
    return hashlib.md5(value.encode()).hexdigest()


class Fixture(http.server.BaseHTTPRequestHandler):
    seen_cookies = []

    def log_message(self, *args):
        pass

    def reply(self, status, body, headers=None):
        self.send_response(status)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def digest_ok(self):
        header = self.headers.get('Authorization', '')
        if not header.startswith('Digest '):
            return False
        fields = {}
        for part in header[len('Digest '):].split(','):
            key, _, value = part.strip().partition('=')
            fields[key] = value.strip('"')
        if fields.get('username') != 'digest-user':
            return False
        ha1 = md5('digest-user:test-realm:digest-pass')
        ha2 = md5(f'GET:{fields.get("uri", "")}')
        expected = md5(':'.join([ha1, 'fixed-nonce', fields.get('nc', ''), fields.get('cnonce', ''),
                                 fields.get('qop', ''), ha2]))
        return fields.get('response') == expected

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/basic':
            expected = 'Basic ' + base64.b64encode(b'basic-user:basic-pass').decode()
            if self.headers.get('Authorization') != expected:
                self.reply(401, b'', {'WWW-Authenticate': 'Basic realm="test"'})
                return
            self.reply(200, PAYLOAD)
            return
        if path == '/bearer':
            if self.headers.get('Authorization') != 'Bearer bearer-token':
                self.reply(401, b'', {'WWW-Authenticate': 'Bearer'})
                return
            self.reply(200, PAYLOAD)
            return
        if path == '/digest':
            if not self.digest_ok():
                self.reply(401, b'', {'WWW-Authenticate':
                    'Digest realm="test-realm", nonce="fixed-nonce", qop="auth", algorithm=MD5'})
                return
            self.reply(200, PAYLOAD)
            return
        if path == '/cross-auth':
            expected = 'Basic ' + base64.b64encode(b'basic-user:basic-pass').decode()
            if self.headers.get('Authorization') != expected:
                self.reply(401, b'', {'WWW-Authenticate': 'Basic realm="test"'})
                return
            self.reply(302, b'', {'Location': self.server.cross_target,
                                  'Set-Cookie': 'crosscookie=1; Path=/'})
            return
        if path == '/cookie/import':
            self.reply(200, self.headers.get('Cookie', '').encode())
            return
        if path == '/cookie/set':
            self.reply(302, b'', {'Location': '/cookie/check',
                                  'Set-Cookie': 'scoped=1; Path=/cookie; Max-Age=60'})
            return
        if path == '/cookie/check':
            if 'scoped=1' in self.headers.get('Cookie', ''):
                self.reply(200, b'scoped-cookie-ok')
            else:
                self.reply(403, b'missing scoped cookie')
            return
        if path == '/cookie/set-outside':
            self.reply(302, b'', {'Location': '/outside/check',
                                  'Set-Cookie': 'narrow=1; Path=/scope; Max-Age=60'})
            return
        if path == '/outside/check':
            if 'narrow=1' in self.headers.get('Cookie', ''):
                self.reply(403, b'path-scoped cookie leaked')
            else:
                self.reply(200, b'path-cookie-ok')
            return
        if path == '/cookie/set-expired':
            self.reply(302, b'', {'Location': '/cookie/expired-check',
                                  'Set-Cookie': 'stale=1; Path=/; Max-Age=0'})
            return
        if path == '/cookie/expired-check':
            if 'stale=1' in self.headers.get('Cookie', ''):
                self.reply(403, b'expired cookie sent')
            else:
                self.reply(200, b'expired-cookie-ok')
            return
        if path == '/cookie/leak':
            Fixture.seen_cookies.append(self.headers.get('Cookie', ''))
            self.reply(200, PAYLOAD)
            return
        self.reply(404, b'')


class CrossTarget(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        body = (b'auth=' + (self.headers.get('Authorization') or '').encode() +
                b';cookie=' + (self.headers.get('Cookie') or '').encode())
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class ProxyFixture(http.server.BaseHTTPRequestHandler):
    requests = []

    def log_message(self, *args):
        pass

    def do_GET(self):
        ProxyFixture.requests.append((self.path, dict(self.headers)))
        expected = 'Basic ' + base64.b64encode(b'proxy-user:proxy-secret').decode()
        if self.headers.get('Proxy-Authorization') != expected:
            self.send_response(407)
            self.send_header('Proxy-Authenticate', 'Basic realm="proxy"')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        body = b'proxied payload'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


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
        self.environment = {key: value for key, value in os.environ.items()
                            if key.lower() not in ('http_proxy', 'https_proxy', 'all_proxy')}
        self.environment.update(no_proxy='127.0.0.1,127.0.0.2,localhost',
                                NO_PROXY='127.0.0.1,127.0.0.2,localhost')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.process = None

    def start(self):
        self.process = subprocess.Popen([BINARY, '--config', str(self.config)], env=self.environment,
                                        stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 3
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

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()

    def api(self, suffix, body=None, method=None):
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/tasks{suffix}',
            data=None if body is None else json.dumps(body).encode(), method=method,
            headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=3) as response:
            raw = response.read()
            return json.loads(raw) if raw else {}

    def profile(self, kind, body):
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/{kind}',
            data=json.dumps(body).encode(), method='POST', headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=3) as response:
            return json.loads(response.read())

    def task(self, url, name, extra=None):
        body = {'url': url, 'file_name': name, 'target_directory': str(self.downloads)}
        if extra is not None:
            body['extra'] = extra
        task = self.api('', body)['task']
        self.api('/' + task['id'] + '/start', {})
        return task

    def wait(self, task, states=('completed', 'failed'), timeout=10):
        deadline = time.monotonic() + timeout
        current = None
        while time.monotonic() < deadline:
            current = self.api('/' + task['id'])['task']
            if current['state'] in states:
                return current
            time.sleep(.01)
        raise AssertionError(f'task deadline exceeded: {current}')

    def contents(self, task):
        return (self.downloads / task['file_name']).read_bytes()


def expect_success(daemon, url, name, expected, extra=None):
    task = daemon.task(url, name, extra)
    current = daemon.wait(task)
    assert current['state'] == 'completed', current
    assert daemon.contents(task) == expected, current
    daemon.api('/' + task['id'], method='DELETE')


def expect_failure(daemon, url, name, extra=None):
    task = daemon.task(url, name, extra)
    current = daemon.wait(task)
    assert current['state'] == 'failed', current
    assert current['error_message'], current
    daemon.api('/' + task['id'], method='DELETE')
    return current


def main():
    if len(sys.argv) != 2 or not pathlib.Path(sys.argv[1]).is_absolute():
        raise SystemExit('usage: python3 tests/test-http-auth.py /absolute/path/downloader')
    global BINARY
    BINARY = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='http-auth-') as directory:
        root = pathlib.Path(directory)
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
        server.daemon_threads = True
        cross = http.server.ThreadingHTTPServer(('127.0.0.2', 0), CrossTarget)
        cross.daemon_threads = True
        server.cross_target = f'http://127.0.0.2:{cross.server_port}/capture'
        threading.Thread(target=cross.serve_forever, kwargs={'poll_interval': .02}, daemon=True).start()
        threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .02}, daemon=True).start()
        proxy = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ProxyFixture)
        proxy.daemon_threads = True
        threading.Thread(target=proxy.serve_forever, kwargs={'poll_interval': .02}, daemon=True).start()
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            closed_port = probe.getsockname()[1]
        base = f'http://127.0.0.1:{server.server_port}'
        daemon = Daemon(BINARY, root)
        try:
            daemon.start()
            daemon.profile('auth-profiles', {'name': 'basic-profile', 'kind': 'basic',
                                             'username': 'basic-user', 'secret': 'basic-pass'})
            daemon.profile('auth-profiles', {'name': 'bearer-profile', 'kind': 'bearer',
                                             'secret': 'bearer-token'})
            daemon.profile('auth-profiles', {'name': 'digest-profile', 'kind': 'digest',
                                             'username': 'digest-user', 'secret': 'digest-pass'})
            daemon.profile('auth-profiles', {'name': 'wrong-basic', 'kind': 'basic',
                                             'username': 'basic-user', 'secret': 'wrong'})
            daemon.profile('auth-profiles', {'name': 'cookie-profile', 'kind': 'cookie',
                                             'secret': 'BDUSS=session; STOKEN=netdisk'})
            daemon.profile('proxy-profiles', {'name': 'proxy-profile',
                                              'url': f'http://127.0.0.1:{proxy.server_port}',
                                              'username': 'proxy-user', 'secret': 'proxy-secret'})
            daemon.profile('proxy-profiles', {'name': 'wrong-proxy',
                                              'url': f'http://127.0.0.1:{proxy.server_port}',
                                              'username': 'proxy-user', 'secret': 'wrong'})

            expect_success(daemon, base + '/basic', 'basic.bin', PAYLOAD,
                           {'auth_profile': 'basic-profile'})
            print('PASS Basic authentication', flush=True)
            expect_success(daemon, base + '/bearer', 'bearer.bin', PAYLOAD,
                           {'auth_profile': 'bearer-profile'})
            print('PASS Bearer authentication', flush=True)
            expect_success(daemon, base + '/digest', 'digest.bin', PAYLOAD,
                           {'auth_profile': 'digest-profile'})
            print('PASS Digest authentication', flush=True)
            expect_failure(daemon, base + '/basic', 'basic-wrong.bin', {'auth_profile': 'wrong-basic'})
            print('PASS wrong credentials fail without retry fallback', flush=True)

            current = expect_failure(daemon, base + '/cookie/leak', 'cookie-leak.bin',
                                     {'auth_profile': 'cookie-profile'})
            assert 'not supported' in current['error_message'].lower(), current
            assert not any('BDUSS=' in cookie for cookie in Fixture.seen_cookies)
            print('PASS cookie credentials are rejected by http adapters', flush=True)

            expect_success(daemon, base + '/cookie/import', 'cookie-alpha.bin', b'account=alpha',
                           {'http': {'cookies': ['account=alpha; Path=/']}})
            expect_success(daemon, base + '/cookie/import', 'cookie-beta.bin', b'account=beta',
                           {'http': {'cookies': ['account=beta; Path=/']}})
            print('PASS imported cookie accounts stay isolated', flush=True)
            expect_success(daemon, base + '/cookie/set', 'cookie-set.bin', b'scoped-cookie-ok')
            expect_success(daemon, base + '/cookie/set-outside', 'cookie-path.bin', b'path-cookie-ok')
            expect_success(daemon, base + '/cookie/set-expired', 'cookie-expired.bin', b'expired-cookie-ok')
            print('PASS server cookies honor path and expiry', flush=True)

            expect_success(daemon, base + '/cross-auth', 'cross.bin', b'auth=;cookie=',
                           {'auth_profile': 'basic-profile'})
            print('PASS cross-host redirect drops authorization and cookies', flush=True)

            target = f'http://127.0.0.1:{closed_port}/proxied.bin'
            expect_success(daemon, target, 'proxy.bin', b'proxied payload',
                           {'proxy_profile': 'proxy-profile'})
            assert ProxyFixture.requests, 'proxy was bypassed'
            assert any(request[1].get('Proxy-Authorization') for request in ProxyFixture.requests)
            expect_failure(daemon, target, 'proxy-wrong.bin', {'proxy_profile': 'wrong-proxy'})
            # A name that cannot resolve locally still succeeds via the proxy,
            # proving DNS and transport go through the proxy.
            expect_success(daemon, 'http://unresolvable.invalid/proxied-dns.bin', 'proxy-dns.bin',
                           b'proxied payload', {'proxy_profile': 'proxy-profile'})
            print('PASS authenticated proxy is mandatory (no direct fallback)', flush=True)
        finally:
            daemon.stop()
            for running in (proxy, cross, server):
                running.shutdown()
                running.server_close()


if __name__ == '__main__':
    main()
