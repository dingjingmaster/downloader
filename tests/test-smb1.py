#!/usr/bin/env python3
"""SMB1 compatibility backend test against a local Samba NT1 server.

Usage: test-smb1.py --daemon /abs/path/downloader
Skips (return 77) when smbd or openssl is unavailable.
"""
import argparse
import hashlib
import json
import os
import pathlib
import pwd
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

LARGE_SIZE = 64 * 1024 * 1024


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def wait_port(port, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                return True
        except OSError:
            time.sleep(.05)
    return False


class Daemon:
    def __init__(self, binary, root):
        self.root = root
        self.downloads = root / 'downloads'
        self.downloads.mkdir()
        self.port = free_port()
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

    def wait(self, task, states=('completed', 'failed'), timeout=40):
        deadline = time.monotonic() + timeout
        current = None
        while time.monotonic() < deadline:
            current = self.api('/' + task['id'])['task']
            if current['state'] in states:
                return current
            time.sleep(.02)
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--daemon', required=True)
    args = parser.parse_args()
    if shutil.which('smbd') is None or shutil.which('openssl') is None:
        print('SKIP: smbd or openssl not available')
        return 77
    user = pwd.getpwuid(os.getuid()).pw_name
    password = 'Fixture:Smb1@1'
    port = free_port()
    root = pathlib.Path(tempfile.mkdtemp(prefix='downloader-smb1-'))
    fixture = root / 'samba'
    for name in ('share', 'private', 'state', 'cache', 'lock', 'pid', 'ncalrpc'):
        (fixture / name).mkdir(parents=True)
    small = b'smb1 compat payload\n'
    (fixture / 'share/small.bin').write_bytes(small)
    with open(fixture / 'share/large.bin', 'wb') as large:
        large.truncate(LARGE_SIZE)
    large_hash = hashlib.sha256(bytes(LARGE_SIZE)).hexdigest()
    digest = subprocess.run(['openssl', 'dgst', '-md4', '-provider', 'legacy'],
                            input=password.encode('utf-16le'), capture_output=True, check=True)
    nt_hash = digest.stdout.decode().strip().split()[-1].upper()
    passdb = fixture / 'private/smbpasswd'
    passdb.write_text(f'{user}:{os.getuid()}:' + 'X' * 32 + f':{nt_hash}:[U          ]:LCT-{int(time.time()):08X}:\n')
    passdb.chmod(0o600)
    config = fixture / 'smb.conf'
    config.write_text(f'''[global]
server role = standalone server
security = user
workgroup = WORKGROUP
log level = 1
interfaces = 127.0.0.1
bind interfaces only = yes
smb ports = {port}
server min protocol = NT1
server max protocol = NT1
load printers = no
printing = bsd
printcap name = /dev/null
disable spoolss = yes
private dir = {fixture}/private
state directory = {fixture}/state
cache directory = {fixture}/cache
lock directory = {fixture}/lock
pid directory = {fixture}/pid
ncalrpc dir = {fixture}/ncalrpc
log file = {fixture}/log.%m
passdb backend = smbpasswd:{fixture}/private/smbpasswd
[share]
path = {fixture}/share
read only = yes
guest ok = no
''')
    smbd_log = open(fixture / 'smbd.log', 'w+')
    smbd = subprocess.Popen(['smbd', '-F', '--no-process-group', '--debug-stdout', '-s', str(config)],
                            stdin=subprocess.DEVNULL, stdout=smbd_log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    daemon = None
    try:
        if not wait_port(port):
            smbd_log.seek(0)
            raise AssertionError('fixture smbd did not listen: ' + smbd_log.read())
        daemon = Daemon(args.daemon, root)
        daemon.ready()
        base = f'smb1://{urllib.parse.quote(user, safe="")}:{urllib.parse.quote(password, safe="")}@127.0.0.1:{port}/share'

        task = daemon.create(base + '/small.bin', 'small.bin')
        daemon.api('/' + task['id'] + '/start', {})
        current = daemon.wait(task)
        assert current['state'] == 'completed', current
        assert (daemon.downloads / 'small.bin').read_bytes() == small
        print('PASS SMB1 download through libsmbclient', flush=True)

        bad = daemon.create(f'smb1://{user}:wrong-password@127.0.0.1:{port}/share/small.bin', 'bad.bin')
        daemon.api('/' + bad['id'] + '/start', {})
        current = daemon.wait(bad)
        assert current['state'] == 'failed', current
        assert 'SMB1' in current['error_message'], current
        print('PASS SMB1 authentication failure does not fall back', flush=True)

        task = daemon.create(base + '/large.bin', 'large.bin')
        daemon.api('/' + task['id'] + '/start', {})
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if daemon.api('/' + task['id'])['task']['state'] == 'downloading':
                break
            time.sleep(.01)
        daemon.api('/' + task['id'] + '/pause', {})
        paused = daemon.wait(task, ('paused', 'completed'))
        if paused['state'] == 'completed':
            print('SKIP: 64 MiB transfer completed before pause could be delivered')
        else:
            partial = daemon.downloads / f'.{task["id"]}.part'
            assert partial.exists(), 'paused SMB1 task lost its partial file'
            daemon.api('/' + task['id'] + '/start', {})
            current = daemon.wait(task)
            assert current['state'] == 'completed', current
            assert hashlib.sha256((daemon.downloads / 'large.bin').read_bytes()).hexdigest() == large_hash
            print('PASS SMB1 pause and restart', flush=True)
        daemon.api('/' + task['id'], method='DELETE')
        assert not (daemon.downloads / f'.{task["id"]}.part').exists()

        task = daemon.create(base + '/large.bin', 'delete.bin')
        daemon.api('/' + task['id'] + '/start', {})
        time.sleep(.02)
        daemon.api('/' + task['id'], method='DELETE')
        assert not (daemon.downloads / f'.{task["id"]}.part').exists()
        assert not (daemon.downloads / 'delete.bin').exists()
        print('PASS SMB1 active delete removes the partial file', flush=True)
    finally:
        if daemon is not None:
            daemon.close()
        try:
            os.killpg(smbd.pid, 15)
        except ProcessLookupError:
            pass
        smbd.wait(timeout=5)
        smbd_log.close()
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
