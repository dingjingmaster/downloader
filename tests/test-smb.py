#!/usr/bin/env python3
"""Standalone SMB adapter tests; no CMake changes or installed services.

Run: python3 tests/test-smb.py [--build-dir build-vendor] [--sanitize]
Requires built bundled libsmb2/clib archives, cc, smbd and OpenSSL with its legacy MD4 provider.
CTest registration (parent-owned): Python3 interpreter + this script, with
--build-dir ${CMAKE_BINARY_DIR} --daemon $<TARGET_FILE:downloader>;
use TIMEOUT 120 and SKIP_RETURN_CODE 77.
The fixture binds loopback on a high port and stores all Samba state in /tmp.
"""
import argparse
import errno
import json
import select
import socketserver
import threading
import urllib.request
import os
from pathlib import Path
import pwd
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from urllib.parse import quote

HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include "smb.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ProtocolTask* active;
static int progressCount;
static int stopMode;
static int failures;
static void progress (void* unused, uint64_t total, uint64_t downloaded, uint64_t speed)
{
    (void)unused;
    (void)total;
    (void)downloaded;
    (void)speed;
    ++progressCount;
    if (stopMode == 7 && downloaded >= 65536) {
        FILE* remote = fopen (getenv ("SMB_TEST_REMOTE"), "r+b");
        assert (remote != NULL);
        assert (fputc ('X', remote) != EOF);
        assert (fclose (remote) == 0);
        stopMode = 0;
    }
    if (stopMode == 1 || (stopMode == 3 && progressCount >= 10) || (stopMode == 6 && downloaded >= 65536))
        assert (smb_protocol_ops ()->pause (active) == 0);
    if (stopMode == 2 || (stopMode == 4 && progressCount >= 10))
        assert (smb_protocol_ops ()->cancel (active) == 0);
}
static void failed (void* unused, int code, const char* message)
{
    (void)unused;
    assert (code > 0);
    assert (strstr (message, "secret") == NULL);
    ++failures;
}
int main (int argc, char** argv)
{
    const ProtocolOps* ops = smb_protocol_ops ();
    ProtocolCallbacks callbacks = {.onProgress = progress, .onFailed = failed};
    DownloadTask task = {0};
    assert (strcmp (ops->name, "smb") == 0);
    assert (ops->canHandle ("smb://localhost/share/file", "smb"));
    assert (!ops->canHandle ("http://localhost/file", "http"));
    assert (ops->open (NULL, &task, &callbacks, NULL) == -1 && errno == EINVAL);
    snprintf (task.protocol, sizeof (task.protocol), "smb");
    snprintf (task.tempPath, sizeof (task.tempPath), "/tmp/smb-adapter-test-%ld", (long)getpid ());
    snprintf (task.resourceUrl, sizeof (task.resourceUrl), "%s", argc > 1 ? argv[1] : "smb://user:secret@127.0.0.1:1/share/file");
    if (argc > 2)
        snprintf (task.tempPath, sizeof (task.tempPath), "%s", argv[2]);
    stopMode = argc > 3 ? atoi (argv[3]) : 1;
    int opened = ops->open (&active, &task, &callbacks, NULL);
    if (stopMode == 5) {
        assert (opened == -1 && errno == EINVAL);
        return 0;
    }
    assert (opened == 0);
    if (argc == 1)
        assert (access (task.tempPath, F_OK) == -1);
    int result = ops->run (active);
    if (stopMode == 6) {
        assert (result == 1);
        assert (ops->resume (active) == 0);
        stopMode = 0;
        result = ops->run (active);
    }
    int saved = errno;
    ops->close (active);
    assert (progressCount > 0);
    printf ("result=%d errno=%d progress=%d failures=%d\n", result, saved, progressCount, failures);
    fflush (stdout);
    if (stopMode == 1 || stopMode == 3)
        assert (result == 1 && failures == 0);
    else if (stopMode == 2 || stopMode == 4)
        assert (result == -1 && saved == ECANCELED);
    else if (argc > 4)
        assert (result == -1 && saved == atoi (argv[4]));
    else
        assert (result == 0);
    return 0;
}
'''


def daemon_tests(binary, fixture, smb_port, base, remote, payload):
    unicode_remote = remote
    remote = fixture / "share/daemon name %.bin"
    remote.write_bytes(payload)
    downloads = fixture / "downloads"
    downloads.mkdir()
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        web_port = probe.getsockname()[1]
    config = fixture / "download.ini"
    config.write_text(f"[daemon]\nipc_socket={fixture}/ipc.sock\n[download]\n"
                      f"database={fixture}/meta.db\ndownload_dir={downloads}\n"
                      f"[web]\nlisten=127.0.0.1\nport={web_port}\n")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    log = (fixture / "daemon.log").open("w+")
    daemon = subprocess.Popen([str(Path(binary).resolve()), "--config", str(config)],
                              stdout=log, stderr=log)
    gate = threading.Event()
    connected = threading.Event()
    shutdown = threading.Event()

    class Relay(socketserver.BaseRequestHandler):
        def handle(self):
            connected.set()
            while not gate.wait(.05):
                if shutdown.is_set():
                    return
            try:
                with socket.create_connection(("127.0.0.1", smb_port), timeout=2) as upstream:
                    sockets = [self.request, upstream]
                    while not shutdown.is_set():
                        readable, _, _ = select.select(sockets, [], [], .1)
                        for incoming in readable:
                            chunk = incoming.recv(65536)
                            if not chunk:
                                return
                            destination = upstream if incoming is self.request else self.request
                            destination.sendall(chunk)
            except OSError:
                pass

    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True
        allow_reuse_address = True

    relay = Server(("127.0.0.1", 0), Relay)
    relay_thread = threading.Thread(target=relay.serve_forever, daemon=True)
    relay_thread.start()

    def api(path="", body=None, method=None):
        request = urllib.request.Request(f"http://127.0.0.1:{web_port}/api/tasks{path}",
            data=None if body is None else json.dumps(body).encode(),
            headers={"Content-Type": "application/json"}, method=method)
        with opener.open(request, timeout=5) as response:
            return json.load(response)

    created_count = 0

    def create(url):
        nonlocal created_count
        created_count += 1
        task = api("", {"url": url, "target_directory": str(downloads / str(created_count))})["task"]
        assert task["protocol"] == "smb"
        return task

    def wait(task, states=("completed", "failed")):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            current = api("/" + task["id"])["task"]
            if current["state"] in states:
                return current
            time.sleep(.02)
        raise AssertionError("SMB daemon task did not reach " + repr(states))

    def verify(task):
        current = wait(task)
        assert current["state"] == "completed", current.get("error_message")
        result = Path(current["target_directory"]) / current["file_name"]
        assert result.read_bytes() == payload
        assert current["total_bytes"] == current["downloaded_bytes"] == len(payload)
        assert not (Path(task["target_directory"]) / f'.{task["id"]}.part').exists()
        return current

    try:
        for _ in range(150):
            try:
                api()
                break
            except OSError:
                if daemon.poll() is not None:
                    raise AssertionError("downloader exited during startup")
                time.sleep(.02)
        else:
            raise AssertionError("downloader web API did not start")
        url = base + "/share/" + quote(remote.name, safe="")
        task = create(url)
        assert task["file_name"] == remote.name, (task["file_name"], remote.name)
        api("/" + task["id"] + "/start", {})
        verify(task)
        api("/" + task["id"], method="DELETE")
        assert (Path(task["target_directory"]) / remote.name).read_bytes() == payload
        print("PASS daemon SMB create/start/decoded-name/atomic completion/delete-completed")

        relay_url = base.rsplit(":", 1)[0] + f":{relay.server_address[1]}/share/" + quote(remote.name, safe="")
        task = create(relay_url)
        api("/" + task["id"] + "/start", {})
        assert connected.wait(3)
        started = time.monotonic()
        api("/" + task["id"] + "/pause", {})
        assert wait(task, ("paused",))["state"] == "paused"
        assert time.monotonic() - started < 3
        partial = Path(task["target_directory"]) / f'.{task["id"]}.part'
        # Seed a genuine prefix while paused; stat, not stale DB bytes, controls resume.
        partial.write_bytes(payload[:12345])
        time.sleep(.15)
        assert partial.stat().st_size == 12345
        gate.set()
        api("/" + task["id"] + "/start", {})
        verify(task)
        print("PASS daemon SMB pause during connect and resume from stat prefix")

        gate.clear()
        connected.clear()
        task = create(relay_url)
        api("/" + task["id"] + "/start", {})
        assert connected.wait(3)
        started = time.monotonic()
        api("/" + task["id"], method="DELETE")
        assert time.monotonic() - started < 3
        assert not (Path(task["target_directory"]) / f'.{task["id"]}.part').exists()
        print("PASS daemon SMB active delete during connect")

        bad = base.split(":", 1)[0] + "://" + pwd.getpwuid(os.getuid()).pw_name
        bad += f":wrongsecret@127.0.0.1:{smb_port}/private/empty"
        task = create(bad)
        api("/" + task["id"] + "/start", {})
        current = wait(task)
        assert current["state"] == "failed"
        assert current["error_message"] and "secret" not in current["error_message"]
        print("PASS daemon SMB authentication failure redaction")
        unicode_task = create(base + "/share/" + quote(unicode_remote.name, safe=""))
        assert unicode_task["file_name"] == unicode_remote.name, (unicode_task["file_name"], unicode_remote.name)
        api("/" + unicode_task["id"] + "/start", {})
        verify(unicode_task)
        print("PASS daemon SMB Unicode decoded name")
    finally:
        shutdown.set()
        gate.set()
        relay.shutdown()
        relay.server_close()
        relay_thread.join(timeout=3)
        daemon.terminate()
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
            raise AssertionError("downloader failed to stop")
        finally:
            log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build-vendor")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--smb2-only", action="store_true", help="cap fixture dialect to SMB2_10")
    parser.add_argument("--daemon", help="also test a built downloader through its Web API")
    parser.add_argument("--daemon-only", action="store_true", help="run just Web integration checks")
    args = parser.parse_args()
    if args.daemon_only and not args.daemon:
        parser.error("--daemon-only requires --daemon")
    root = Path(__file__).resolve().parents[1]
    build = (root / args.build_dir).resolve()
    for tool in ("cc", "smbd", "openssl"):
        if not shutil.which(tool):
            print("SKIP: missing", tool)
            return 77
    clib = build / "clib/libclib.a"
    smb = build / "3thrd/installed/lib/libsmb2.a"
    if not clib.exists() or not smb.exists():
        print("SKIP: build bundled clib/libsmb2 first")
        return 77
    with tempfile.TemporaryDirectory(prefix="downloader-smb-") as directory:
        fixture = Path(directory)
        harness = fixture / "adapter.c"
        harness.write_text(HARNESS)
        binary = fixture / "adapter"
        krb5 = subprocess.run(["pkg-config", "--libs", "krb5"], capture_output=True, text=True)
        command = ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g",
                   "-I" + str(root / "protocols"), "-I" + str(root / "daemon"),
                   "-I" + str(build / "clib/include"),
                   "-I" + str(root / "3thrd/libsmb2/include"),
                   str(harness), str(root / "protocols/smb.c"), str(clib), str(smb),
                   "-pthread", "-lanl", "-o", str(binary)]
        command[command.index("-o"):command.index("-o")] = (
            krb5.stdout.split() if krb5.returncode == 0 else []) + ["-lgssapi_krb5"]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        for folder in ("share", "private", "state", "cache", "lock", "pid", "ncalrpc"):
            (fixture / folder).mkdir()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        user = pwd.getpwuid(os.getuid()).pw_name
        password = "fixture:secret@%"
        config = fixture / "smb.conf"
        config.write_text(f"""[global]
server role = standalone server
log level = 1
workgroup = WORKGROUP
security = user
map to guest = Bad User
guest account = {user}
interfaces = 127.0.0.1
bind interfaces only = yes
smb ports = {port}
server min protocol = SMB2
server max protocol = {"SMB2_10" if args.smb2_only else "SMB3"}
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
guest ok = yes
[private]
path = {fixture}/share
read only = yes
guest ok = no
""")
        # smbpasswd -L requires root even with a private config. Generate only
        # the fixture NT hash, then write Samba's documented smbpasswd format.
        digest = subprocess.run(["openssl", "dgst", "-md4", "-provider", "legacy"],
                                input=password.encode("utf-16le"), capture_output=True, check=True)
        nt_hash = digest.stdout.decode().strip().split()[-1].upper()
        passdb = fixture / "private/smbpasswd"
        passdb.write_text(f"{user}:{os.getuid()}:" + "X" * 32 + f":{nt_hash}:[U          ]:LCT-{int(time.time()):08X}:\n")
        passdb.chmod(0o600)
        # The system log directory is root-only on some hosts; keep smbd's
        # bootstrap logging on the fixture's stdout instead. A transient
        # startup failure is retried before giving up.
        server = None
        output = None
        failure = ""
        for attempt in range(3):
            output = open(fixture / f"smbd-{attempt}.log", "w+")
            server = subprocess.Popen(["smbd", "-F", "--no-process-group", "--debug-stdout",
                                       "-s", str(config)],
                                      stdin=subprocess.DEVNULL, stdout=output,
                                      stderr=subprocess.STDOUT, start_new_session=True)
            ready = False
            started = time.monotonic()
            while time.monotonic() - started < 5:
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        ready = True
                        break
                except OSError:
                    # smbd may let the initial -F process exit while a child
                    # is still binding the port; allow a short grace period.
                    if server.poll() is not None and time.monotonic() - started >= 2:
                        break
                    time.sleep(.05)
            if ready:
                break
            output.seek(0)
            failure = output.read()
            output.close()
            try:
                os.killpg(server.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            server.wait(timeout=5)
            server = None
        if server is None:
            raise RuntimeError("fixture smbd failed to start after retries: " + failure)
        try:
            base = f"smb://{quote(user)}:{quote(password, safe='')}@127.0.0.1:{port}"
            target = fixture / "result.part"
            count = 0

            def run(url, mode=0, expected=None):
                nonlocal count
                cmd = [str(binary), url, str(target), str(mode)]
                if expected is not None:
                    cmd.append(str(expected))
                # Do not print argv: it contains test credentials.
                completed = subprocess.run(cmd, capture_output=True, text=True, timeout=40,
                                           env=dict(os.environ, SMB_TEST_REMOTE=str(remote)))
                if completed.returncode:
                    raise AssertionError(completed.stdout + completed.stderr + "\n" + "\n".join(p.read_text(errors="replace") for p in fixture.glob("log.*"))[-5000:])
                count += 1
                print("PASS", count, completed.stdout.strip())

            payload = bytes(range(256)) * 1200
            remote = fixture / "share/name % 雪.bin"
            remote.write_bytes(payload)
            url = base + "/share/" + quote(remote.name, safe="")
            if args.daemon_only:
                daemon_tests(args.daemon, fixture, port, base, remote, payload)
                return 0
            run(url)
            assert target.read_bytes() == payload
            target.write_bytes(payload[:12345])
            run(url)
            assert target.read_bytes() == payload
            target.write_bytes(b"stale" * 100)
            run(url)
            assert target.read_bytes() == payload
            target.write_bytes(b"x" * (len(payload) + 1))
            run(url)
            assert target.read_bytes() == payload
            target.write_bytes(b"x" * len(payload))
            run(url)
            assert target.read_bytes() == payload
            target.unlink()
            run(url, mode=6)  # pause during reads, resume using the same adapter
            assert target.read_bytes() == payload
            target.unlink()
            run(url, mode=7, expected=errno.ESTALE)
            assert target.stat().st_size == 0
            remote.write_bytes(payload)
            run(url)
            run(url)  # already-complete prefix is still validated
            assert target.read_bytes() == payload
            (fixture / "share/empty").touch()
            run(base + "/share/empty")
            assert target.stat().st_size == 0
            run(f"smb://127.0.0.1:{port}/share/" + quote(remote.name, safe=""))
            assert target.read_bytes() == payload
            run(base + "/private/" + quote(remote.name, safe=""))
            assert target.read_bytes() == payload
            run(base + "/share/missing", expected=errno.ENOENT)
            (fixture / "share/directory").mkdir()
            run(base + "/share/directory", expected=errno.EISDIR)
            run(f"smb://{user}:wrongsecret@127.0.0.1:{port}/private/empty", expected=errno.EACCES)
            run(base + "/missing/empty", expected=errno.ENOENT)
            target.unlink()
            protected = fixture / "protected.bin"
            protected.write_bytes(b"must not change")
            os.link(protected, target)
            run(url, expected=errno.EMLINK)
            assert protected.read_bytes() == b"must not change"
            target.unlink()
            target.symlink_to(remote)
            run(url, expected=errno.ELOOP)
            assert remote.read_bytes() == payload
            target.unlink()
            for suffix in ("/share/a%00b", "/share/a%GG", "/share/", "/share/file?password=secret"):
                run(base + suffix, mode=5)
            run(url, mode=1)
            run(url, mode=2)
            # A TCP peer accepts SMB but never responds: exercise connect waits.
            with socket.socket() as stalled:
                stalled.bind(("127.0.0.1", 0))
                stalled.listen(4)
                dead = f"smb://127.0.0.1:{stalled.getsockname()[1]}/share/file"
                started = time.monotonic()
                run(dead, mode=3)
                run(dead, mode=4)
                assert time.monotonic() - started < 3
                run(dead, expected=errno.ETIMEDOUT)
            if args.daemon:
                daemon_tests(args.daemon, fixture, port, base, remote, payload)
            print(f"SMB fixture: {count} cases passed; content comparisons passed")
        finally:
            try:
                os.killpg(server.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            server.wait(timeout=5)
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
