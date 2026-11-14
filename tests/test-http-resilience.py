#!/usr/bin/env python3
"""Run with: python3 tests/test-http-resilience.py /absolute/path/to/downloader.

Only stdlib and loopback HTTP are used. Schema contract (daemon/database.c):
 download_tasks.uuid TEXT identifies API id; temp_path/target_path TEXT are paths;
 state TEXT, error_code/downloaded_bytes/total_bytes/retry_count/max_retries INTEGER;
 etag/last_modified TEXT contain literal HTTP validators. Fixtures update only
 inactive, API-created rows; no fabricated schema or live transfer SQL writes.
 Normal config: max_retries=2, connect_timeout=1, transfer_timeout=0.
 Timeout fixture: transfer_timeout=1, including all attempts and backoff.
"""

import contextlib
import errno
import http.server
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request


PAYLOAD = bytes(range(256)) * 257 + b'end-of-resilience-fixture'
PREFIX = PAYLOAD[:8192]
ETAG = '"fixture-v1"'
MODIFIED = 'Wed, 21 Oct 2015 07:28:00 GMT'
CHECKPOINT_INTERVAL = 1024 * 1024
CRASH_PAYLOAD = bytes(range(256)) * 32768
BINARY = None


class Route:
    def __init__(self, kind, **options):
        self.kind = kind
        self.options = options
        self.url = None
        self.requests = []
        self.lock = threading.Lock()
        self.responded = threading.Event()
        self.release = threading.Event()

    def record(self, headers):
        with self.lock:
            self.requests.append((time.monotonic(), dict(headers)))
            return len(self.requests)

    def snapshot(self):
        with self.lock:
            return list(self.requests)


class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        route = self.server.routes.get(self.path)
        if route is None:
            self.send_error(404)
            return
        attempt = route.record(self.headers)
        kind = route.kind
        options = route.options
        try:
            if kind == 'stalled-error' and attempt == 1:
                status = options['status']
                self.send_response(status)
                self.send_header('Content-Length', '65536')
                if status == 503:
                    self.send_header('Retry-After', '1')
                elif status == 416:
                    self.send_header('Content-Range', 'bytes */0')
                self.end_headers()
                self.wfile.flush()
                route.responded.set()
                # No body bytes and no EOF until the test has checked the
                # terminal state. Default transfer_timeout=0 must not hang.
                route.release.wait()
                return
            if kind in ('transient', 'backoff', 'timeout'):
                if kind == 'backoff' or attempt <= options.get('failures', 1):
                    if kind == 'timeout':
                        # Spend most of the *shared* budget on attempt one.
                        route.release.wait(.70)
                    self.reply(options.get('status', 503), b'', {
                        'Retry-After': str(options.get('delay', 1))})
                    route.responded.set()
                    return
                if kind == 'timeout':
                    # Headers arrive, body stalls until teardown explicitly releases it.
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(PAYLOAD)))
                    self.end_headers()
                    route.responded.set()
                    route.release.wait(5)
                    return
            if kind == 'forbidden':
                self.reply(403, b'forbidden')
                return
            if kind in ('redirect', 'loop', 'file-redirect'):
                location = self.path if kind == 'loop' else options['location']
                self.reply(302, b'', {'Location': location, 'ETag': '"redirect-poison"',
                           'Last-Modified': 'Tue, 20 Oct 2015 07:28:00 GMT',
                           'Content-Disposition': 'attachment; filename="poison.bin"'})
                return
            if kind == 'range416' and (attempt == 1 or options.get('always')):
                self.reply(416, b'', {'Content-Range': 'bytes */0'})
                return
            headers = {'ETag': ETAG, 'Last-Modified': MODIFIED}
            content = options.get('payload', PAYLOAD)
            body = content
            status = 200
            requested = self.headers.get('Range')
            if kind == 'changed':
                headers['ETag'] = '"fixture-v2"'
                body = PAYLOAD[::-1]
            elif kind == 'unsolicited':
                status = 206
                body = PAYLOAD[:1024]
                headers['Content-Range'] = f'bytes 0-1023/{len(PAYLOAD)}'
            elif requested and kind not in ('ignore', 'range416'):
                offset = int(requested.split('=', 1)[1].split('-', 1)[0])
                if kind == 'wrong-offset':
                    offset += 1
                status = 206
                body = content[offset:]
                headers['Content-Range'] = f'bytes {offset}-{len(content)-1}/{len(content)}'
            if kind == 'disposition':
                extended = "filename*=UTF-8''preferred%20name.bin"
                fallback = 'filename="fallback.bin"'
                fields = (extended, fallback) if options['extended_first'] else (fallback, extended)
                headers['Content-Disposition'] = 'attachment; ' + '; '.join(fields)
            if kind == 'pause200' and attempt == 1:
                self.send_response(200)
                for key, value in headers.items():
                    self.send_header(key, value)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body[:8192])
                self.wfile.flush()
                route.responded.set()
                route.release.wait(5)
                self.wfile.write(body[8192:])
                return
            if kind == 'slowrange':
                self.send_response(status)
                for key, value in headers.items():
                    self.send_header(key, value)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                for index in range(0, len(body), 65536):
                    self.wfile.write(body[index:index + 65536])
                    self.wfile.flush()
                    time.sleep(.02)
                return
            if kind == 'drop' and attempt <= options.get('failures', 1):
                self.send_response(status)
                for key, value in headers.items():
                    self.send_header(key, value)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body[:1024])
                self.wfile.flush()
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_WR)
                return
            self.reply(status, body, headers)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def reply(self, status, body, headers=None):
        self.send_response(status)
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()


class Daemon:
    def __init__(self, binary, timeout=0, max_retries=2):
        self.binary = binary
        self.temporary = tempfile.TemporaryDirectory(prefix='http-resilience-')
        self.root = pathlib.Path(self.temporary.name)
        self.process = None
        self.log = open(self.root / 'daemon.log', 'w+b')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.config = self.root / 'daemon.ini'
        self.config.write_text(
            f'[daemon]\nipc_socket={self.root}/ipc.sock\n'
            f'[download]\ndatabase={self.root}/meta.db\ndownload_dir={self.root}\n'
            f'max_retries={max_retries}\nconnect_timeout=1\ntransfer_timeout={timeout}\n'
            f'[web]\nlisten=127.0.0.1\nport={self.port}\n', encoding='utf-8')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def __enter__(self):
        try:
            self.start()
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, *args):
        try:
            self.stop()
        finally:
            self.log.close()
            self.temporary.cleanup()

    def start(self):
        environment = dict(os.environ, no_proxy='127.0.0.1', NO_PROXY='127.0.0.1')
        self.process = subprocess.Popen([self.binary, '--config', str(self.config)],
                                        env=environment, stdout=self.log, stderr=self.log)
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
                raise AssertionError('daemon shutdown exceeded 3 seconds')

    def api(self, suffix, body=None, method=None):
        request = urllib.request.Request(
            f'http://127.0.0.1:{self.port}/api/tasks{suffix}',
            data=None if body is None else json.dumps(body).encode(), method=method,
            headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=2) as response:
            raw = response.read()
            return json.loads(raw) if raw else {}

    def row(self, identifier):
        with sqlite3.connect(self.root / 'meta.db', timeout=2) as database:
            database.row_factory = sqlite3.Row
            return dict(database.execute('SELECT * FROM download_tasks WHERE uuid=?',
                                         (identifier,)).fetchone())

    def checkpoint(self, identifier):
        with sqlite3.connect(self.root / 'meta.db', timeout=2) as database:
            database.row_factory = sqlite3.Row
            try:
                row = database.execute('SELECT * FROM task_checkpoints WHERE task_uuid=?',
                                       (identifier,)).fetchone()
            except sqlite3.OperationalError:
                # Pre-checkpoint schemas have no table; tests fail on the value.
                return None
            return dict(row) if row else None

    def kill(self):
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=3)

    def seed(self, identifier, etag='', modified=''):
        row = self.row(identifier)
        assert row['state'] in ('created', 'paused'), row
        pathlib.Path(row['temp_path']).write_bytes(PREFIX)
        with sqlite3.connect(self.root / 'meta.db', timeout=2) as database:
            database.execute(
                'UPDATE download_tasks SET state=?, downloaded_bytes=?, total_bytes=?, '
                'etag=?, last_modified=? WHERE uuid=?',
                ('paused', len(PREFIX), len(PAYLOAD), etag, modified, identifier))

    def wait(self, identifier, states=("completed", "failed"), timeout=10):
        deadline = time.monotonic() + timeout
        task = None
        while time.monotonic() < deadline:
            task = self.api('/' + identifier)['task']
            if task['state'] in states:
                return task
            time.sleep(.01)
        raise AssertionError(f'task deadline exceeded: {task}')


class Resilience(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.resources = contextlib.ExitStack()
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
        cls.server.daemon_threads = True
        cls.server.routes = {}
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      kwargs={'poll_interval': .02}, daemon=True)
        cls.thread.start()
        cls.resources.callback(cls.server.server_close)
        cls.resources.callback(cls.server.shutdown)
        try:
            cls.daemon = cls.resources.enter_context(Daemon(BINARY))
        except BaseException:
            cls.resources.close()
            raise
        cls.serial = 0

    @classmethod
    def tearDownClass(cls):
        for route in cls.server.routes.values():
            route.release.set()
        cls.resources.close()
        cls.thread.join(timeout=1)

    def setUp(self):
        self.created = []

    def tearDown(self):
        # Failed assertions must not leave workers affecting subsequent cases.
        for daemon, identifier in self.created:
            try:
                daemon.api('/' + identifier, method='DELETE')
            except urllib.error.HTTPError as error:
                if error.code != 404:
                    raise

    def route(self, kind, **options):
        type(self).serial += 1
        path = f'/fixture-{self.serial}'
        route = Route(kind, **options)
        route.url = f'http://127.0.0.1:{self.server.server_port}{path}'
        self.server.routes[path] = route
        return route.url, route

    def create(self, kind, daemon=None, extra=None, auto_name=False, **options):
        daemon = daemon or self.daemon
        url, route = self.route(kind, **options)
        destination = daemon.root / f'task-{self.serial}'
        destination.mkdir()
        body = {'url': url, 'target_directory': str(destination)}
        if not auto_name:
            body['file_name'] = f'result-{self.serial}.bin'
        if extra is not None:
            body['extra'] = extra
        task = daemon.api('', body)['task']
        self.created.append((daemon, task['id']))
        return task['id'], route

    def run_task(self, identifier, daemon=None):
        daemon = daemon or self.daemon
        daemon.api('/' + identifier + '/start', {})
        return daemon.wait(identifier)

    def complete(self, identifier, task, expected=PAYLOAD, daemon=None):
        daemon = daemon or self.daemon
        self.assertEqual(task['state'], 'completed', task)
        row = daemon.row(identifier)
        self.assertEqual(pathlib.Path(row['target_path']).read_bytes(), expected)
        self.assertFalse(pathlib.Path(row['temp_path']).exists())
        self.assertEqual(task['downloaded_bytes'], len(expected))

    def failed(self, identifier, task, daemon=None):
        daemon = daemon or self.daemon
        self.assertEqual(task['state'], 'failed', task)
        row = daemon.row(identifier)
        self.assertIs(type(row['error_code']), int)
        self.assertNotEqual(row['error_code'], 0, row)
        self.assertFalse(pathlib.Path(row['target_path']).exists())

    def resume(self, etag, modified):
        identifier, route = self.create('range')
        self.daemon.seed(identifier, etag, modified)
        # A real daemon restart proves validators were loaded from SQLite.
        self.daemon.stop()
        self.daemon.start()
        self.complete(identifier, self.run_task(identifier))
        requests = route.snapshot()
        self.assertEqual(len(requests), 1)
        self.assertEqual(requests[0][1].get('Range'), f'bytes={len(PREFIX)}-')
        self.assertEqual(requests[0][1].get('If-Range'), etag or modified)

    def test_resume_persisted_etag(self):
        self.resume(ETAG, MODIFIED)

    def test_resume_persisted_last_modified(self):
        self.resume('', MODIFIED)

    def test_partial_without_validator_restarts(self):
        identifier, route = self.create('range')
        self.daemon.seed(identifier)
        self.complete(identifier, self.run_task(identifier))
        self.assertEqual(len(route.snapshot()), 1)
        self.assertNotIn('Range', route.snapshot()[0][1])
        self.assertNotIn('If-Range', route.snapshot()[0][1])

    def reset_response(self, kind, expected):
        identifier, route = self.create(kind)
        self.daemon.seed(identifier, ETAG)
        self.complete(identifier, self.run_task(identifier), expected)
        self.assertEqual(len(route.snapshot()), 1)
        self.assertEqual(route.snapshot()[0][1].get('If-Range'), ETAG)
        self.assertEqual(route.snapshot()[0][1].get('Range'), f'bytes={len(PREFIX)}-')
        if kind == 'changed':
            self.assertEqual(self.daemon.row(identifier)['etag'], '"fixture-v2"')

    def test_ignored_range_200_resets(self):
        self.reset_response('ignore', PAYLOAD)

    def test_changed_etag_200_resets(self):
        self.reset_response('changed', PAYLOAD[::-1])

    def test_wrong_206_offset_preserves_prefix(self):
        identifier, route = self.create('wrong-offset')
        self.daemon.seed(identifier, ETAG)
        self.failed(identifier, self.run_task(identifier))
        self.assertEqual(pathlib.Path(self.daemon.row(identifier)['temp_path']).read_bytes(), PREFIX)
        self.assertEqual(len(route.snapshot()), 1)

    def test_416_restarts_without_range(self):
        identifier, route = self.create('range416')
        self.daemon.seed(identifier, ETAG)
        self.complete(identifier, self.run_task(identifier))
        requests = route.snapshot()
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0][1].get('Range'), f'bytes={len(PREFIX)}-')
        self.assertNotIn('Range', requests[1][1])
        self.assertNotIn('If-Range', requests[1][1])

    def test_416_restart_is_bounded(self):
        identifier, route = self.create('range416', always=True)
        self.daemon.seed(identifier, ETAG)
        self.failed(identifier, self.run_task(identifier))
        self.assertGreaterEqual(len(route.snapshot()), 2)
        self.assertLessEqual(len(route.snapshot()), 3)

    def test_416_recovers_once_with_zero_max_retries(self):
        with Daemon(BINARY, max_retries=0) as daemon:
            identifier, route = self.create('range416', daemon=daemon)
            try:
                daemon.seed(identifier, ETAG)
                task = self.run_task(identifier, daemon)
                self.complete(identifier, task, daemon=daemon)
                requests = route.snapshot()
                self.assertEqual(len(requests), 2)
                self.assertEqual(requests[0][1].get('Range'), f'bytes={len(PREFIX)}-')
                self.assertEqual(requests[0][1].get('If-Range'), ETAG)
                self.assertNotIn('Range', requests[1][1])
                self.assertNotIn('If-Range', requests[1][1])
                self.assertEqual(daemon.row(identifier)['max_retries'], 0)
                self.assertEqual(task['retry_count'], 0, '416 recovery consumed retry budget')
            finally:
                self.created.remove((daemon, identifier))
                daemon.api('/' + identifier, method='DELETE')

    def test_redirect_resume_same_etag_restarts_full_representation(self):
        # The persisted prefix belongs to A. B deliberately reuses A's ETag;
        # validators are scoped to resources, so B's suffix cannot be appended.
        replacement = PAYLOAD[::-1]
        final_url, final_route = self.route('range', payload=replacement)
        identifier, redirect_route = self.create('redirect', location=final_url)
        self.daemon.seed(identifier, ETAG, MODIFIED)
        self.complete(identifier, self.run_task(identifier), replacement)
        redirects = redirect_route.snapshot()
        finals = final_route.snapshot()
        self.assertEqual(redirects[0][1].get('Range'), f'bytes={len(PREFIX)}-')
        self.assertEqual(redirects[0][1].get('If-Range'), ETAG)
        # Implementations may reject the redirect before or after following it,
        # but must issue a full request to B and stay bounded.
        self.assertGreaterEqual(len(redirects), 1)
        self.assertLessEqual(len(redirects), 2)
        self.assertGreaterEqual(len(finals), 1)
        self.assertLessEqual(len(finals), 2)
        self.assertNotIn('Range', finals[-1][1])
        self.assertNotIn('If-Range', finals[-1][1])
        # The full request to B becomes the new representation identity.
        self.assertEqual(self.daemon.row(identifier)['etag'], ETAG)
        self.assertEqual(self.daemon.row(identifier)['last_modified'], MODIFIED)

    def test_pause_200_preserves_total_bytes(self):
        identifier, route = self.create('pause200')
        try:
            self.daemon.api('/' + identifier + '/start', {})
            self.assertTrue(route.responded.wait(2), '200 prefix was not sent')
            deadline = time.monotonic() + 2
            while True:
                current = self.daemon.api('/' + identifier)['task']
                if current['downloaded_bytes'] >= 8192:
                    break
                self.assertLess(time.monotonic(), deadline, current)
                time.sleep(.01)
            self.assertEqual(current['total_bytes'], len(PAYLOAD), current)
            self.daemon.api('/' + identifier + '/pause', {})
            paused = self.daemon.wait(identifier, ('paused',), timeout=2)
            self.assertEqual(paused['total_bytes'], len(PAYLOAD), paused)
            self.assertEqual(paused['downloaded_bytes'], 8192, paused)
            row = self.daemon.row(identifier)
            self.assertEqual(row['total_bytes'], len(PAYLOAD))
            self.assertEqual(pathlib.Path(row['temp_path']).read_bytes(), PREFIX)
            self.assertFalse(pathlib.Path(row['target_path']).exists())
            self.assertEqual(len(route.snapshot()), 1)
            checkpoint = self.daemon.checkpoint(identifier)
            self.assertIsNotNone(checkpoint)
            self.assertEqual(checkpoint['version'], 1)
            self.assertEqual(checkpoint['durable_bytes'], len(PREFIX))
            self.assertEqual(checkpoint['effective_url'], route.url)
            self.assertEqual(checkpoint['etag'], ETAG)
            self.assertEqual(checkpoint['last_modified'], MODIFIED)
        finally:
            route.release.set()
        self.complete(identifier, self.run_task(identifier))
        self.assertEqual(len(route.snapshot()), 2)

    def test_unsolicited_partial_206_never_publishes(self):
        identifier, route = self.create('unsolicited')
        self.failed(identifier, self.run_task(identifier))
        self.assertEqual(len(route.snapshot()), 1)

    def transient(self, status):
        identifier, route = self.create('transient', status=status)
        task = self.run_task(identifier)
        self.complete(identifier, task)
        requests = route.snapshot()
        self.assertEqual(len(requests), 2)
        self.assertGreaterEqual(requests[1][0] - requests[0][0], .90, 'Retry-After ignored')
        self.assertEqual(task['retry_count'], 1)
        self.assertEqual(self.daemon.row(identifier)['retry_count'], 1)
        self.daemon.stop()
        self.daemon.start()
        self.assertEqual(self.daemon.api('/' + identifier)['task']['retry_count'], 1)

    def test_503_retry_after_and_persistence(self):
        self.transient(503)

    def test_429_retry_after_and_persistence(self):
        self.transient(429)

    def recovered_after_manager_retry(self, kind, **options):
        # The adapter's burst is limited by max_retries, then the manager keeps
        # retrying forever; the fixture recovers on the fourth response.
        identifier, route = self.create(kind, failures=4, **options)
        task = self.run_task(identifier)
        self.complete(identifier, task)
        self.assertGreaterEqual(task['retry_count'], 3, task)
        self.assertGreaterEqual(len(route.snapshot()), 4)
        row = self.daemon.row(identifier)
        self.assertGreaterEqual(row['retry_count'], 3)
        self.assertEqual(row['max_retries'], 2)

    def test_503_recovers_after_manager_retry(self):
        self.recovered_after_manager_retry('transient', status=503, delay=0)

    def test_429_recovers_after_manager_retry(self):
        self.recovered_after_manager_retry('transient', status=429, delay=0)

    def test_dropped_transfer_recovers_after_manager_retry(self):
        self.recovered_after_manager_retry('drop')

    def test_dropped_transfer_retries_with_validator(self):
        identifier, route = self.create('drop')
        task = self.run_task(identifier)
        self.complete(identifier, task)
        requests = route.snapshot()
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[1][1].get('Range'), 'bytes=1024-')
        self.assertEqual(requests[1][1].get('If-Range'), ETAG)
        self.assertEqual(task['retry_count'], 1)

    def test_403_numeric_error_without_retry(self):
        identifier, route = self.create('forbidden')
        task = self.run_task(identifier)
        self.failed(identifier, task)
        self.assertIn('403', task['error_message'])
        self.assertEqual(task['retry_count'], 0)
        self.assertEqual(len(route.snapshot()), 1)

    def stalled_error(self, status):
        identifier, route = self.create('stalled-error', status=status)
        try:
            if status == 416:
                self.daemon.seed(identifier, ETAG)
            self.daemon.api('/' + identifier + '/start', {})
            self.assertTrue(route.responded.wait(2), 'error headers were not sent')
            started = time.monotonic()
            # Allow the 503 Retry-After=1 delay, but never release the body
            # merely to let the task finish. A broken adapter times out here.
            task = self.daemon.wait(identifier, timeout=2.5)
            self.assertLess(time.monotonic() - started, 2.5,
                            'task waited for the stalled error body')
            self.assertFalse(route.release.is_set())
            requests = route.snapshot()
            if status == 403:
                self.failed(identifier, task)
                self.assertIn('403', task['error_message'])
                self.assertEqual(task['retry_count'], 0)
                self.assertEqual(len(requests), 1)
            else:
                self.complete(identifier, task)
                self.assertEqual(len(requests), 2)
                self.assertNotIn('Range', requests[1][1])
                self.assertNotIn('If-Range', requests[1][1])
                if status == 503:
                    self.assertEqual(task['retry_count'], 1)
                    self.assertGreaterEqual(requests[1][0] - requests[0][0], .90,
                                            'Retry-After ignored')
                else:
                    self.assertEqual(task['retry_count'], 0)
                    self.assertEqual(requests[0][1].get('Range'), f'bytes={len(PREFIX)}-')
                    self.assertEqual(requests[0][1].get('If-Range'), ETAG)
        finally:
            route.release.set()

    def test_403_stalled_body_fails_after_headers(self):
        self.stalled_error(403)

    def test_503_stalled_body_retries_after_headers(self):
        self.stalled_error(503)

    def test_416_stalled_body_recovers_after_headers(self):
        self.stalled_error(416)

    def cancel_backoff(self, delete):
        identifier, route = self.create('backoff', delay=30)
        self.daemon.api('/' + identifier + '/start', {})
        self.assertTrue(route.responded.wait(2), 'server did not send Retry-After')
        # retry_count establishes that the adapter consumed the retryable response.
        deadline = time.monotonic() + 1
        while self.daemon.api('/' + identifier)['task'].get('retry_count', 0) == 0:
            self.assertLess(time.monotonic(), deadline, 'backoff was not entered')
            time.sleep(.01)
        row = self.daemon.row(identifier)
        started = time.monotonic()
        if delete:
            self.daemon.api('/' + identifier, method='DELETE')
            with self.assertRaises(urllib.error.HTTPError) as caught:
                self.daemon.api('/' + identifier)
            self.assertEqual(caught.exception.code, 404)
            self.created.remove((self.daemon, identifier))
            self.assertFalse(pathlib.Path(row['temp_path']).exists())
        else:
            self.daemon.api('/' + identifier + '/pause', {})
            self.daemon.wait(identifier, ('paused',), timeout=1)
        self.assertLess(time.monotonic() - started, 1.0, 'cancel blocked on Retry-After')
        self.assertEqual(len(route.snapshot()), 1)
        self.assertFalse(pathlib.Path(row['target_path']).exists())

    def test_pause_during_backoff_is_prompt(self):
        self.cancel_backoff(False)

    def test_delete_during_backoff_is_prompt(self):
        self.cancel_backoff(True)

    def test_transfer_timeout_is_overall_and_retried(self):
        with Daemon(BINARY, timeout=1) as daemon:
            identifier, route = self.create('timeout', daemon=daemon, delay=1)
            try:
                started = time.monotonic()
                daemon.api('/' + identifier + '/start', {})
                deadline = time.monotonic() + 3
                current = None
                while time.monotonic() < deadline:
                    current = daemon.api('/' + identifier)['task']
                    if current['retry_count'] >= 1:
                        break
                    time.sleep(.02)
                elapsed = time.monotonic() - started
                self.assertIsNotNone(current)
                self.assertGreaterEqual(current['retry_count'], 1, current)
                self.assertIn(current['state'], ('queued', 'downloading'), current)
                self.assertGreaterEqual(elapsed, .6)
                self.assertLess(elapsed, 2.5, 'the timeout was not retried promptly')
                self.assertTrue(route.responded.is_set(), 'first response never arrived')
                # A retrying task can still be paused promptly.
                started = time.monotonic()
                daemon.api('/' + identifier + '/pause', {})
                daemon.wait(identifier, ('paused',), timeout=2)
                self.assertLess(time.monotonic() - started, 1.0)
            finally:
                route.release.set()
                daemon.api('/' + identifier, method='DELETE')
                self.created.remove((daemon, identifier))

    def test_redirect_persists_only_final_metadata(self):
        final_url, final_route = self.route('range')
        identifier, route = self.create('redirect', location=final_url, auto_name=True)
        initial_name = self.daemon.row(identifier)['file_name']
        task = self.run_task(identifier)
        self.complete(identifier, task)
        row = self.daemon.row(identifier)
        self.assertEqual(row['file_name'], initial_name)
        self.assertEqual(row['etag'], ETAG)
        self.assertEqual(row['last_modified'], MODIFIED)
        self.assertFalse((pathlib.Path(row['target_directory']) / 'poison.bin').exists())
        self.assertEqual(len(route.snapshot()), 1)
        self.assertEqual(len(final_route.snapshot()), 1)
        self.assertIsNone(self.daemon.checkpoint(identifier))

    def test_redirect_partial_then_direct_same_etag_restarts(self):
        final_url, final_route = self.route('pause200')
        identifier, entry_route = self.create('redirect', location=final_url)
        try:
            self.daemon.api('/' + identifier + '/start', {})
            self.assertTrue(final_route.responded.wait(2), 'redirected prefix was not sent')
            deadline = time.monotonic() + 2
            while True:
                current = self.daemon.api('/' + identifier)['task']
                if current['downloaded_bytes'] >= len(PREFIX):
                    break
                self.assertLess(time.monotonic(), deadline, current)
                time.sleep(.01)
            self.daemon.api('/' + identifier + '/pause', {})
            self.daemon.wait(identifier, ('paused',), timeout=2)
            row = self.daemon.row(identifier)
            checkpoint = self.daemon.checkpoint(identifier)
            self.assertEqual(pathlib.Path(row['temp_path']).read_bytes(), PREFIX)
            self.assertIsNotNone(checkpoint)
            self.assertEqual(checkpoint['effective_url'], final_url)
            self.assertEqual(checkpoint['etag'], ETAG)
            self.assertEqual(checkpoint['last_modified'], MODIFIED)
            self.assertEqual(len(entry_route.snapshot()), 1)
            self.assertEqual(len(final_route.snapshot()), 1)
        finally:
            final_route.release.set()
        # With the worker paused, change the entry resource to a direct 200/206
        # server for B. Its ETag is identical to A's but its bytes differ.
        entry_route.kind = 'range'
        entry_route.options = {'payload': PAYLOAD[::-1]}
        self.daemon.stop()
        self.daemon.start()
        self.complete(identifier, self.run_task(identifier), PAYLOAD[::-1])
        requests = entry_route.snapshot()
        # The persisted identity still points at the old final URL, so the
        # resumed attempt is rejected after headers and the retry is full.
        self.assertEqual(len(requests), 3)
        self.assertEqual(requests[1][1].get('Range'), f'bytes={len(PREFIX)}-')
        self.assertEqual(requests[1][1].get('If-Range'), ETAG)
        self.assertNotIn('Range', requests[2][1])
        self.assertNotIn('If-Range', requests[2][1])
        self.assertEqual(len(final_route.snapshot()), 1)

    def pause_redirected(self, final_route, identifier):
        self.daemon.api('/' + identifier + '/start', {})
        self.assertTrue(final_route.responded.wait(2), 'redirected prefix was not sent')
        deadline = time.monotonic() + 2
        while True:
            current = self.daemon.api('/' + identifier)['task']
            if current['downloaded_bytes'] >= len(PREFIX):
                break
            self.assertLess(time.monotonic(), deadline, current)
            time.sleep(.01)
        self.daemon.api('/' + identifier + '/pause', {})
        self.daemon.wait(identifier, ('paused',), timeout=2)
        checkpoint = self.daemon.checkpoint(identifier)
        self.assertIsNotNone(checkpoint)
        self.assertEqual(checkpoint['durable_bytes'], len(PREFIX))
        self.assertEqual(checkpoint['etag'], ETAG)
        self.assertEqual(checkpoint['last_modified'], MODIFIED)
        return checkpoint

    def test_redirect_pause_resumes_on_same_final_url(self):
        # The persisted identity is the final URL, so a redirect that still
        # lands on the same resource may append the prefix.
        final_url, final_route = self.route('pause200')
        identifier, entry_route = self.create('redirect', location=final_url)
        try:
            checkpoint = self.pause_redirected(final_route, identifier)
            self.assertEqual(checkpoint['effective_url'], final_url)
            self.assertEqual(len(entry_route.snapshot()), 1)
            self.assertEqual(len(final_route.snapshot()), 1)
        finally:
            final_route.release.set()
        self.daemon.stop()
        self.daemon.start()
        self.complete(identifier, self.run_task(identifier))
        self.assertEqual(len(entry_route.snapshot()), 2)
        self.assertEqual(len(final_route.snapshot()), 2)
        resumed = final_route.snapshot()[-1][1]
        self.assertEqual(resumed.get('Range'), f'bytes={len(PREFIX)}-')
        self.assertEqual(resumed.get('If-Range'), ETAG)

    def test_redirect_target_change_same_etag_restarts(self):
        # A different final URL with the same ETag is a different resource.
        first_url, first_route = self.route('pause200')
        identifier, entry_route = self.create('redirect', location=first_url)
        try:
            checkpoint = self.pause_redirected(first_route, identifier)
            self.assertEqual(checkpoint['effective_url'], first_url)
        finally:
            first_route.release.set()
        replacement = PAYLOAD[::-1]
        second_url, second_route = self.route('range', payload=replacement)
        entry_route.options = {'location': second_url}
        self.daemon.stop()
        self.daemon.start()
        self.complete(identifier, self.run_task(identifier), replacement)
        finals = second_route.snapshot()
        self.assertGreaterEqual(len(finals), 1)
        self.assertLessEqual(len(finals), 2)
        self.assertNotIn('Range', finals[-1][1])
        self.assertNotIn('If-Range', finals[-1][1])

    def test_long_effective_url_downloads_without_checkpoint(self):
        # A redirect target longer than the task URL column cannot be compared
        # on resume, but the download itself must still succeed.
        path = '/long-' + 'a' * 4100
        final_route = Route('range')
        final_url = f'http://127.0.0.1:{self.server.server_port}{path}'
        final_route.url = final_url
        self.server.routes[path] = final_route
        identifier, entry_route = self.create('redirect', location=final_url)
        self.complete(identifier, self.run_task(identifier))
        self.assertEqual(len(entry_route.snapshot()), 1)
        self.assertEqual(len(final_route.snapshot()), 1)
        self.assertIsNone(self.daemon.checkpoint(identifier))

    def test_failure_before_headers_preserves_checkpoint_identity(self):
        identifier, route = self.create('pause200')
        try:
            self.daemon.api('/' + identifier + '/start', {})
            self.assertTrue(route.responded.wait(2), 'prefix was not sent')
            deadline = time.monotonic() + 2
            while True:
                current = self.daemon.api('/' + identifier)['task']
                if current['downloaded_bytes'] >= len(PREFIX):
                    break
                self.assertLess(time.monotonic(), deadline, current)
                time.sleep(.01)
            self.daemon.api('/' + identifier + '/pause', {})
            self.daemon.wait(identifier, ('paused',), timeout=2)
        finally:
            route.release.set()
        checkpoint = self.daemon.checkpoint(identifier)
        self.assertEqual(checkpoint['durable_bytes'], len(PREFIX))
        self.assertEqual(checkpoint['effective_url'], route.url)
        # The next run fails before any 2xx response. It must not erase the
        # identity against which the valid prefix was proven.
        route.kind = 'transient'
        route.options = {'status': 503, 'failures': 99, 'delay': 0}
        self.daemon.api('/' + identifier + '/start', {})
        deadline = time.monotonic() + 6
        current = None
        while time.monotonic() < deadline:
            current = self.daemon.api('/' + identifier)['task']
            if current['state'] == 'queued':
                break
            time.sleep(.02)
        self.assertEqual(current['state'], 'queued', current)
        checkpoint = self.daemon.checkpoint(identifier)
        self.assertIsNotNone(checkpoint)
        self.assertEqual(checkpoint['effective_url'], route.url)
        self.assertEqual(checkpoint['durable_bytes'], len(PREFIX))
        self.daemon.api('/' + identifier, method='DELETE')
        self.created.remove((self.daemon, identifier))

    def test_crash_resumes_from_durable_checkpoint(self):
        identifier, route = self.create('slowrange', payload=CRASH_PAYLOAD)
        self.daemon.api('/' + identifier + '/start', {})
        deadline = time.monotonic() + 10
        checkpoint = None
        while time.monotonic() < deadline:
            checkpoint = self.daemon.checkpoint(identifier)
            if checkpoint and checkpoint['durable_bytes'] >= CHECKPOINT_INTERVAL:
                break
            time.sleep(.01)
        self.assertIsNotNone(checkpoint, 'no durable checkpoint was written')
        self.assertGreaterEqual(checkpoint['durable_bytes'], CHECKPOINT_INTERVAL)
        self.assertEqual(checkpoint['effective_url'], route.url)
        self.daemon.kill()
        # Read exactly what SQLite committed before the crash.
        checkpoint = self.daemon.checkpoint(identifier)
        self.assertIsNotNone(checkpoint)
        durable = checkpoint['durable_bytes']
        self.assertGreaterEqual(durable, CHECKPOINT_INTERVAL)
        self.assertLess(durable, len(CRASH_PAYLOAD))
        self.daemon.start()
        # The daemon auto-resumes tasks recovered from DOWNLOADING; no manual start.
        self.complete(identifier, self.daemon.wait(identifier), CRASH_PAYLOAD)
        resumed = route.snapshot()[-1][1]
        self.assertEqual(resumed.get('Range'), f'bytes={durable}-')
        self.assertEqual(resumed.get('If-Range'), ETAG)
        self.assertIsNone(self.daemon.checkpoint(identifier))

    def test_redirect_loop_is_bounded(self):
        identifier, route = self.create('loop')
        self.failed(identifier, self.run_task(identifier))
        self.assertGreater(len(route.snapshot()), 1)
        self.assertLessEqual(len(route.snapshot()), 21)

    def test_file_redirect_is_rejected(self):
        sentinel = self.daemon.root / 'redirect-source'
        sentinel.write_bytes(PAYLOAD)
        identifier, route = self.create('file-redirect', location=sentinel.as_uri())
        self.failed(identifier, self.run_task(identifier))
        self.assertEqual(len(route.snapshot()), 1)
        self.assertEqual(sentinel.read_bytes(), PAYLOAD)

    def disposition(self, extended_first):
        identifier, route = self.create('disposition', extended_first=extended_first, auto_name=True)
        self.complete(identifier, self.run_task(identifier))
        self.assertEqual(self.daemon.row(identifier)['file_name'], 'preferred name.bin')
        self.assertEqual(len(route.snapshot()), 1)

    def test_filename_star_first(self):
        self.disposition(True)

    def test_filename_star_last(self):
        self.disposition(False)

    def test_custom_user_agent_and_referer(self):
        identifier, route = self.create('range', extra={'http': {
            'user_agent': 'resilience-test/1.0', 'referer': 'http://127.0.0.1/origin'}})
        self.complete(identifier, self.run_task(identifier))
        requests = route.snapshot()
        self.assertEqual(len(requests), 1)
        self.assertEqual(requests[0][1].get('User-Agent'), 'resilience-test/1.0')
        self.assertEqual(requests[0][1].get('Referer'), 'http://127.0.0.1/origin')

    def malformed(self, key):
        identifier, route = self.create('range', extra={'http': {key: 'bad\r\nX-Injected: yes'}})
        self.failed(identifier, self.run_task(identifier))
        self.assertEqual(route.snapshot(), [], 'invalid option reached network')

    def test_crlf_user_agent_rejected_before_network(self):
        self.malformed('user_agent')

    def test_crlf_referer_rejected_before_network(self):
        self.malformed('referer')


if __name__ == '__main__':
    if len(sys.argv) != 2 or not pathlib.Path(sys.argv[1]).is_absolute():
        sys.exit('usage: python3 tests/test-http-resilience.py /absolute/path/to/downloader')
    BINARY = sys.argv.pop(1)
    unittest.main(verbosity=2)
