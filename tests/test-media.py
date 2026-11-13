"""Local HLS/DASH task tests. ffmpeg/ffprobe are fixture tools, not daemon dependencies."""
import functools
import http.server
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request


def run(*args):
    try:
        return subprocess.check_output(args, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(error.stderr.decode(errors='replace')) from error


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def probe(path):
    return json.loads(run('ffprobe', '-v', 'error', '-protocol_whitelist', 'file,crypto',
                          '-allowed_extensions', 'ALL', '-count_packets', '-show_streams',
                          '-of', 'json', str(path)))['streams']


def main(binary):
    if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
        raise RuntimeError('ffmpeg and ffprobe are required to generate and verify local media fixtures')
    with tempfile.TemporaryDirectory(prefix='downloader-media-') as temporary:
        root = pathlib.Path(temporary)
        source = root / 'source'
        downloads = root / 'downloads'
        source.mkdir()
        downloads.mkdir()
        (source / 'hls').mkdir()
        (source / 'dash').mkdir()
        (source / 'aes').mkdir()
        run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=96x64:rate=10',
            '-f', 'lavfi', '-i', 'sine=frequency=500:sample_rate=48000', '-t', '8',
            '-c:v', 'libx264', '-g', '10', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
            str(source / 'original.mp4'))
        run('ffmpeg', '-v', 'error', '-i', str(source / 'original.mp4'), '-c', 'copy',
            '-f', 'hls', '-hls_time', '1', '-hls_playlist_type', 'vod', str(source / 'hls/stream.m3u8'))
        run('ffmpeg', '-v', 'error', '-i', str(source / 'original.mp4'), '-c', 'copy',
            '-f', 'dash', '-seg_duration', '1', str(source / 'dash/stream.mpd'))
        (source / 'aes/key.bin').write_bytes(bytes(range(16)))
        key_info = root / 'key-info.txt'
        key_info.write_text(f'key.bin\n{source / "aes/key.bin"}\n')
        run('ffmpeg', '-v', 'error', '-i', str(source / 'original.mp4'), '-c', 'copy',
            '-f', 'hls', '-hls_time', '1', '-hls_playlist_type', 'vod',
            '-hls_key_info_file', str(key_info), str(source / 'aes/stream.m3u8'))
        (source / 'hls/master.m3u8').write_text('#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000\nstream.m3u8\n')
        (source / 'bad.m3u8').write_text('#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nmissing.ts\n#EXT-X-ENDLIST\n')
        (source / 'private.m3u8').write_text('#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nfile:///etc/passwd\n#EXT-X-ENDLIST\n')
        slow = threading.Event()
        stalled = threading.Event()
        release = threading.Event()

        class Fixture(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                redirects = {'/entry.m3u8': '/hls/master.m3u8', '/entry.mpd': '/dash/stream.mpd'}
                if self.path in redirects:
                    self.send_response(302)
                    self.send_header('Location', redirects[self.path])
                    self.end_headers()
                    return
                if self.path == '/stall.m3u8':
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
                    self.send_header('Content-Length', '100')
                    self.end_headers()
                    stalled.set()
                    release.wait(10)
                    return
                try:
                    super().do_GET()
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def copyfile(self, incoming, outgoing):
                while chunk := incoming.read(1024):
                    outgoing.write(chunk)
                    outgoing.flush()
                    if slow.is_set():
                        time.sleep(.015)

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
            functools.partial(Fixture, directory=str(source)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        port = free_port()
        config = root / 'download.ini'
        config.write_text(f'[daemon]\nipc_socket={root}/ipc.sock\n[download]\ndatabase={root}/meta.db\n'
                          f'download_dir={downloads}\n[web]\nlisten=127.0.0.1\nport={port}\n')
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        log = (root / 'daemon.log').open('w+')
        process = subprocess.Popen([binary, '--config', str(config)], stdout=log, stderr=log,
                                   env=dict(os.environ, NO_PROXY='127.0.0.1', no_proxy='127.0.0.1'))

        def api(path='', body=None, method=None):
            request = urllib.request.Request(f'http://127.0.0.1:{port}/api/tasks{path}',
                data=None if body is None else json.dumps(body).encode(),
                headers={'Content-Type': 'application/json'}, method=method)
            with opener.open(request, timeout=5) as response:
                return json.load(response)

        def create(path, name):
            task = api('', {'url': f'http://127.0.0.1:{server.server_port}/{path}',
                            'target_directory': str(downloads / name)})['task']
            api('/' + task['id'] + '/start', {})
            return task

        def wait(task, states=('completed', 'failed'), timeout=20):
            deadline = time.monotonic() + timeout
            current = task
            while time.monotonic() < deadline:
                current = api('/' + task['id'])['task']
                if current['state'] in states:
                    return current
                time.sleep(.03)
            raise AssertionError(current)

        try:
            for _ in range(150):
                try:
                    api()
                    break
                except OSError:
                    time.sleep(.02)
            expected = {stream['codec_type']: int(stream['nb_read_packets'])
                        for stream in probe(source / 'original.mp4')}
            for path, protocol, name in [('hls/master.m3u8', 'hls', 'hls'),
                                         ('dash/stream.mpd', 'dash', 'dash'),
                                         ('aes/stream.m3u8', 'hls', 'aes'),
                                         ('entry.m3u8', 'hls', 'hls-redirect'),
                                         ('entry.mpd', 'dash', 'dash-redirect')]:
                task = create(path, name)
                assert task['protocol'] == protocol, task
                current = wait(task)
                assert current['state'] == 'completed', current
                output = pathlib.Path(current['target_directory']) / current['file_name']
                streams = probe(output)
                packets = {stream['codec_type']: int(stream['nb_read_packets']) for stream in streams}
                reference_path = {'entry.m3u8': 'hls/master.m3u8', 'entry.mpd': 'dash/stream.mpd'}.get(path, path)
                reference = {stream['codec_type']: int(stream['nb_read_packets'])
                             for stream in probe(source / reference_path)}
                assert packets == reference, (packets, reference)
                assert packets['video'] == expected['video'], (packets, expected)
                assert current['downloaded_bytes'] == output.stat().st_size, current
                assert current['total_bytes'] == current['downloaded_bytes'], current
                print(protocol, 'actual VOD remux and audio/video packets: PASS')

            slow.set()
            task = create('hls/stream.m3u8', 'pause')
            wait(task, ('downloading',))
            time.sleep(.3)
            api('/' + task['id'] + '/pause', {})
            assert wait(task, ('paused',))['state'] == 'paused'
            partial = pathlib.Path(task['target_directory']) / f'.{task["id"]}.part'
            previous = partial.stat().st_size if partial.exists() else 0
            time.sleep(.2)
            assert (partial.stat().st_size if partial.exists() else 0) == previous
            slow.clear()
            api('/' + task['id'] + '/start', {})
            current = wait(task)
            assert current['state'] == 'completed', current
            output = pathlib.Path(current['target_directory']) / current['file_name']
            assert any(int(stream['nb_read_packets']) == expected['video'] for stream in probe(output))
            print('media pause and clean restart on resume: PASS')

            for path in ('bad.m3u8', 'private.m3u8'):
                current = wait(create(path, path + '-out'))
                assert current['state'] == 'failed', current
                assert current['error_message'], current
            print('missing segments and forbidden local URLs: PASS')

            task = create('stall.m3u8', 'cancel')
            assert stalled.wait(3)
            started = time.monotonic()
            api('/' + task['id'], method='DELETE')
            assert time.monotonic() - started < 4
            assert not (pathlib.Path(task['target_directory']) / f'.{task["id"]}.part').exists()
            print('cancel blocked manifest request: PASS')
        except BaseException:
            log.flush()
            print((root / 'daemon.log').read_text(), file=sys.stderr)
            raise
        finally:
            release.set()
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            log.close()
            server.shutdown()


if __name__ == '__main__':
    main(str(pathlib.Path(sys.argv[1]).resolve()))
