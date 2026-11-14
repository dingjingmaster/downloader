#!/usr/bin/env python3
"""Loopback Kerberos SMB test: local MIT KDC + Samba smbd with a keytab.

Usage: test-smb-kerberos.py --build-dir BUILD --daemon /abs/path/downloader
Skips (return 77) when the KDC/Samba tools are unavailable.
"""
import argparse
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

REALM = 'TEST.LOCAL'


def run(command, env=None, check=True, timeout=20):
    completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout)
    if check and completed.returncode != 0:
        raise RuntimeError(f'{command[0]} failed: {completed.stdout}{completed.stderr}')
    return completed


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
    def __init__(self, binary, root, krb5_config):
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
        self.environment = dict(os.environ, KRB5_CONFIG=str(krb5_config),
                                KRB5CCNAME=f'FILE:{root}/ccache',
                                NO_PROXY='127.0.0.1,localhost', no_proxy='127.0.0.1,localhost')
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

    def task(self, url, name):
        return self.api('', {'url': url, 'file_name': name, 'target_directory': str(self.downloads),
                             'extra': {'smb': {'auth': 'kerberos', 'domain': REALM}}})['task']

    def wait(self, task, states=('completed', 'failed'), timeout=20):
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
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--daemon', required=True)
    args = parser.parse_args()
    for tool in ('krb5kdc', 'kdb5_util', 'kadmin.local', 'smbd'):
        if shutil.which(tool) is None:
            print(f'SKIP: {tool} not available')
            return 77
    user = pwd.getpwuid(os.getuid()).pw_name
    password = 'Fixture:Krb5@1'
    kdc_port = free_port()
    smb_port = free_port()
    root = pathlib.Path(tempfile.mkdtemp(prefix='downloader-krb-'))
    kdc = root / 'kdc'
    smb = root / 'smb'
    kdc.mkdir()
    for name in ('share', 'private', 'state', 'cache', 'lock', 'pid', 'ncalrpc'):
        (smb / name).mkdir(parents=True)
    (smb / 'share/file.bin').write_bytes(b'kerberos payload\n')
    krb5_config = kdc / 'krb5.conf'
    kdc_config = kdc / 'kdc.conf'
    krb5_config.write_text(f'''[libdefaults]
    default_realm = {REALM}
    dns_lookup_kdc = false
    dns_lookup_realm = false
    rdns = false
    udp_preference_limit = 1
    default_ccache_name = FILE:{root}/ccache
[realms]
    {REALM} = {{
        kdc = 127.0.0.1:{kdc_port}
    }}
[domain_realm]
    localhost = {REALM}
    .localhost = {REALM}
''')
    kdc_config.write_text(f'''[kdcdefaults]
    kdc_ports = {kdc_port}
    kdc_tcp_ports = {kdc_port}
[realms]
    {REALM} = {{
        database_name = {kdc}/principal
        key_stash_file = {kdc}/.k5.{REALM}
        acl_file = {kdc}/kadm5.acl
        max_life = 10h
        max_renewable_life = 7d
    }}
''')
    environment = dict(os.environ, KRB5_CONFIG=str(krb5_config), KRB5_KDC_PROFILE=str(kdc_config))
    keytab = kdc / 'samba.keytab'
    kdc_process = None
    smbd_process = None
    daemon = None
    try:
        run(['kdb5_util', 'create', '-s', '-r', REALM, '-P', 'master-password'], env=environment)
        run(['kadmin.local', '-r', REALM, '-q', f'addprinc -pw {password} {user}'], env=environment)
        run(['kadmin.local', '-r', REALM, '-q', 'addprinc -randkey cifs/localhost'], env=environment)
        run(['kadmin.local', '-r', REALM, '-q', f'ktadd -k {keytab} cifs/localhost'], env=environment)
        kdc_process = subprocess.Popen(['krb5kdc', '-n', '-r', REALM, '-P', str(kdc / 'kdc.pid')],
                                       env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_port(kdc_port):
            raise AssertionError('local KDC did not listen')
        smb_config = smb / 'smb.conf'
        smb_config.write_text(f'''[global]
server role = standalone server
security = user
realm = {REALM}
workgroup = TEST
hostname lookups = no
kerberos method = dedicated keytab
dedicated keytab file = {keytab}
log level = 1
interfaces = 127.0.0.1
bind interfaces only = yes
smb ports = {smb_port}
server min protocol = SMB2
server max protocol = SMB3
load printers = no
printing = bsd
printcap name = /dev/null
disable spoolss = yes
private dir = {smb}/private
state directory = {smb}/state
cache directory = {smb}/cache
lock directory = {smb}/lock
pid directory = {smb}/pid
ncalrpc dir = {smb}/ncalrpc
log file = {smb}/log.%m
passdb backend = smbpasswd:{smb}/private/smbpasswd
[share]
path = {smb}/share
read only = yes
guest ok = no
''')
        passdb = smb / 'private/smbpasswd'
        passdb.write_text('')
        passdb.chmod(0o600)
        smbd_log = open(smb / 'smbd.log', 'w+')
        smbd_process = subprocess.Popen(['smbd', '-F', '--no-process-group', '--debug-stdout',
                                         '-s', str(smb_config)],
                                        stdin=subprocess.DEVNULL, stdout=smbd_log,
                                        stderr=subprocess.STDOUT, start_new_session=True, env=environment)
        if not wait_port(smb_port):
            smbd_log.seek(0)
            logs = ''.join(path.read_text(errors='replace') for path in smb.glob('log.*'))
            raise AssertionError(f'fixture smbd did not listen (rc={smbd_process.poll()}): ' +
                                 smbd_log.read() + logs)
        daemon = Daemon(args.daemon, root, krb5_config)
        daemon.ready()
        quoted = urllib.parse.quote(password, safe='')
        url = f'smb://{user}:{quoted}@localhost:{smb_port}/share/file.bin'
        task = daemon.task(url, 'kerberos.bin')
        daemon.api('/' + task['id'] + '/start', {})
        current = daemon.wait(task)
        assert current['state'] == 'completed', current
        assert (daemon.downloads / 'kerberos.bin').read_bytes() == b'kerberos payload\n'
        print('PASS Kerberos SMB download through local KDC', flush=True)

        bad = daemon.task(f'smb://{user}:wrong-password@localhost:{smb_port}/share/file.bin', 'bad.bin')
        daemon.api('/' + bad['id'] + '/start', {})
        current = daemon.wait(bad)
        assert current['state'] == 'failed', current
        assert current['error_message'], current
        print('PASS wrong Kerberos password fails without NTLM downgrade', flush=True)

        urls = [daemon.task(url, f'parallel-{index}.bin') for index in range(2)]
        for parallel in urls:
            daemon.api('/' + parallel['id'] + '/start', {})
        for parallel in urls:
            assert daemon.wait(parallel)['state'] == 'completed', parallel
        print('PASS concurrent Kerberos sessions complete independently', flush=True)
    finally:
        if daemon is not None:
            daemon.close()
        if smbd_process is not None:
            try:
                os.killpg(smbd_process.pid, 15)
            except ProcessLookupError:
                pass
            smbd_process.wait(timeout=5)
        if kdc_process is not None:
            kdc_process.terminate()
            try:
                kdc_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                kdc_process.kill()
                kdc_process.wait()
        shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
