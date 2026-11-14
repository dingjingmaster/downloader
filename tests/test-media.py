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
import urllib.error
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
        (source / 'byterange').mkdir()
        run('ffmpeg', '-v', 'error', '-i', str(source / 'original.mp4'), '-c', 'copy',
            '-f', 'hls', '-hls_time', '1', '-hls_playlist_type', 'vod', '-hls_flags', 'single_file',
            str(source / 'byterange/stream.m3u8'))
        (source / 'live').mkdir()
        run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=96x64:rate=10',
            '-f', 'lavfi', '-i', 'sine=frequency=700:sample_rate=48000', '-t', '20',
            '-c:v', 'libx264', '-g', '10', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
            str(source / 'live-source.mp4'))
        run('ffmpeg', '-v', 'error', '-i', str(source / 'live-source.mp4'), '-c', 'copy',
            '-f', 'hls', '-hls_time', '1', '-hls_playlist_type', 'vod',
            str(source / 'live/stream.m3u8'))
        live_segments = sorted((source / 'live').glob('stream*.ts'))
        live_clock = {'start': None}

        (source / 'rotate').mkdir()
        run('ffmpeg', '-v', 'error', '-i', str(source / 'original.mp4'), '-c', 'copy',
            '-f', 'hls', '-hls_time', '1', '-hls_playlist_type', 'vod',
            str(source / 'rotate/stream.m3u8'))
        rotate_keys = [bytes(range(16)), bytes(range(16, 32))]
        for index, key in enumerate(rotate_keys):
            (source / 'rotate' / f'key{index + 1}.bin').write_bytes(key)
        rotate_segments = sorted((source / 'rotate').glob('*.ts'))
        for index, segment in enumerate(rotate_segments):
            key = rotate_keys[0] if index < len(rotate_segments) // 2 else rotate_keys[1]
            encrypted = segment.with_suffix('.ts.enc')
            run('openssl', 'enc', '-aes-128-cbc', '-K', key.hex(), '-iv', index.to_bytes(16, 'big').hex(),
                '-nosalt', '-in', str(segment), '-out', str(encrypted))
            encrypted.replace(segment)
        rotate_lines = ['#EXTM3U', '#EXT-X-VERSION:3', '#EXT-X-TARGETDURATION:1',
                        '#EXT-X-MEDIA-SEQUENCE:0', '#EXT-X-PLAYLIST-TYPE:VOD']
        for index, segment in enumerate(rotate_segments):
            if index == 0:
                rotate_lines.append('#EXT-X-KEY:METHOD=AES-128,URI="key1.bin"')
            if index == len(rotate_segments) // 2:
                rotate_lines.append('#EXT-X-KEY:METHOD=AES-128,URI="key2.bin"')
            rotate_lines.append('#EXTINF:1.0,')
            rotate_lines.append(segment.name)
        rotate_lines.append('#EXT-X-ENDLIST')
        (source / 'rotate/stream.m3u8').write_text('\n'.join(rotate_lines))
        (source / 'bad.m3u8').write_text('#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nmissing.ts\n#EXT-X-ENDLIST\n')
        (source / 'drm.m3u8').write_text(
            '#EXTM3U\n#EXT-X-TARGETDURATION:1\n'
            '#EXT-X-KEY:METHOD=SAMPLE-AES,URI="https://example.test/key"\n'
            '#EXTINF:1,\nsegment.ts\n#EXT-X-ENDLIST\n')
        (source / 'drm.mpd').write_text(
            '<?xml version="1.0"?><MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static">'
            '<Period><AdaptationSet><ContentProtection '
            'schemeIdUri="urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"/></AdaptationSet></Period></MPD>')
        (source / 'private.m3u8').write_text('#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nfile:///etc/passwd\n#EXT-X-ENDLIST\n')
        slow = threading.Event()
        very_slow = threading.Event()
        stalled = threading.Event()
        release = threading.Event()
        segment_requests = []
        completed_segments = set()

        class Fixture(http.server.SimpleHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path.endswith(('.ts', '.m4s')):
                    segment_requests.append(self.path)
                ranged = self.headers.get('Range')
                if ranged and self.path.endswith('.ts'):
                    target = source / self.path.lstrip('/')
                    if not target.is_file():
                        self.send_error(404)
                        return
                    data = target.read_bytes()
                    start_text, end_text = ranged.removeprefix('bytes=').split('-', 1)
                    start = int(start_text)
                    end = int(end_text) if end_text else len(data) - 1
                    end = min(end, len(data) - 1)
                    if start < 0 or start > end:
                        self.send_error(416)
                        return
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {start}-{end}/{len(data)}')
                    self.send_header('Content-Length', str(end - start + 1))
                    self.end_headers()
                    try:
                        payload = data[start:end + 1]
                        for offset in range(0, len(payload), 1024):
                            self.wfile.write(payload[offset:offset + 1024])
                            self.wfile.flush()
                            if very_slow.is_set():
                                time.sleep(.05)
                            elif slow.is_set():
                                time.sleep(.015)
                        completed_segments.add(self.path)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    return
                if self.path == '/live/stream.m3u8':
                    if live_clock['start'] is None:
                        live_clock['start'] = time.monotonic()
                    elapsed = time.monotonic() - live_clock['start']
                    available = min(len(live_segments), 3 + int(elapsed / .4))
                    first = max(0, available - 4)
                    lines = ['#EXTM3U', '#EXT-X-VERSION:3', '#EXT-X-TARGETDURATION:1',
                             f'#EXT-X-MEDIA-SEQUENCE:{first}']
                    for index in range(first, available):
                        lines += ['#EXTINF:1.0,', f'stream{index}.ts']
                    if available >= len(live_segments):
                        lines.append('#EXT-X-ENDLIST')
                    body = '\n'.join(lines).encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if self.path == '/live/gap.m3u8':
                    body = ('#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n'
                            '#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1.0,\nstream0.ts\n'
                            '#EXTINF:1.0,\nmissing.ts\n#EXTINF:1.0,\nstream2.ts\n').encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/vnd.apple.mpegurl')
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
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
                # A broken pipe means the client stopped early, so this
                # segment is not a completed cache entry.
                while chunk := incoming.read(1024):
                    outgoing.write(chunk)
                    outgoing.flush()
                    if very_slow.is_set():
                        time.sleep(.05)
                    elif slow.is_set():
                        time.sleep(.015)
                if self.path.endswith(('.ts', '.m4s')):
                    completed_segments.add(self.path)

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

        def reset_live():
            live_clock['start'] = time.monotonic()

        def create(path, name, extra=None):
            body = {'url': f'http://127.0.0.1:{server.server_port}/{path}',
                    'target_directory': str(downloads / name)}
            if extra is not None:
                body['extra'] = extra
            task = api('', body)['task']
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
                                         ('rotate/stream.m3u8', 'hls', 'aes-rotate'),
                                         ('byterange/stream.m3u8', 'hls', 'byterange'),
                                         ('entry.m3u8', 'hls', 'hls-redirect'),
                                         ('entry.mpd', 'dash', 'dash-redirect')]:
                task = create(path, name)
                assert task['protocol'] == protocol, task
                current = wait(task)
                assert current['state'] == 'completed', current
                output = pathlib.Path(current['target_directory']) / current['file_name']
                streams = probe(output)
                packets = {stream['codec_type']: int(stream['nb_read_packets']) for stream in streams}
                reference_path = {'entry.m3u8': 'hls/master.m3u8', 'entry.mpd': 'dash/stream.mpd',
                                  'rotate/stream.m3u8': 'original.mp4',
                                  'byterange/stream.m3u8': 'original.mp4'}.get(path, path)
                reference = {stream['codec_type']: int(stream['nb_read_packets'])
                             for stream in probe(source / reference_path)}
                assert packets == reference, (packets, reference)
                assert packets['video'] == expected['video'], (packets, expected)
                assert current['downloaded_bytes'] == output.stat().st_size, current
                assert current['total_bytes'] == current['downloaded_bytes'], current
                print(protocol, 'actual VOD remux and audio/video packets: PASS')

            def pause_and_resume(path, name, total_segments):
                segment_requests.clear()
                slow.set()
                task = create(path, name)
                wait(task, ('downloading',))
                partial = pathlib.Path(task['target_directory']) / f'.{task["id"]}.part'
                cache_dir = pathlib.Path(str(partial) + '.segments')
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    finished = [item for item in cache_dir.iterdir()
                                if item.is_file() and not item.name.endswith('.part')] \
                        if cache_dir.is_dir() else []
                    if len(finished) >= 4:
                        break
                    time.sleep(.02)
                assert cache_dir.is_dir(), f'segment cache missing: {cache_dir}'
                cached_count = len([item for item in cache_dir.iterdir()
                                    if item.is_file() and not item.name.endswith('.part')])
                assert cached_count >= 4, f'segments were not persisted before pause: {cached_count}'
                cachedSegments = cached_count
                api('/' + task['id'] + '/pause', {})
                assert wait(task, ('paused',))['state'] == 'paused'
                previous = partial.stat().st_size if partial.exists() else 0
                time.sleep(.2)
                assert (partial.stat().st_size if partial.exists() else 0) == previous
                segment_requests.clear()
                slow.clear()
                api('/' + task['id'] + '/start', {})
                current = wait(task)
                assert current['state'] == 'completed', current
                refetched = len(set(segment_requests))
                assert 0 < refetched <= total_segments - cachedSegments, (refetched, cachedSegments)
                assert not cache_dir.exists(), 'segment cache remains after commit'
                output = pathlib.Path(current['target_directory']) / current['file_name']
                assert any(int(stream['nb_read_packets']) == expected['video'] for stream in probe(output))
                api('/' + task['id'], method='DELETE')

            pause_and_resume('hls/stream.m3u8', 'pause', len(list((source / 'hls').glob('*.ts'))))
            print('HLS pause/resume reuses completed segments: PASS')

            very_slow.set()
            task = create('hls/stream.m3u8', 'vod-finish')
            wait(task, ('downloading',))
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if api('/' + task['id'])['task']['downloaded_bytes'] > 0:
                    break
                time.sleep(.02)
            try:
                api('/' + task['id'] + '/finish', {})
            except urllib.error.HTTPError:
                pass
            slow.clear()
            current = wait(task, timeout=30)
            assert current['state'] == 'completed', current
            output = pathlib.Path(current['target_directory']) / current['file_name']
            assert any(int(stream['nb_read_packets']) == expected['video'] for stream in probe(output)), \
                'VOD finish published a truncated recording'
            assert current['duration_seconds'] == 0, current
            api('/' + task['id'], method='DELETE')
            print('VOD finish never publishes a truncated recording: PASS')
            pause_and_resume('dash/stream.mpd', 'pause-dash',
                             len(list((source / 'dash').glob('*.m4s'))))
            print('DASH pause/resume reuses init/segments: PASS')

            for path in ('bad.m3u8', 'private.m3u8'):
                current = wait(create(path, path + '-out'))
                assert current['state'] == 'failed', current
                assert current['error_message'], current
                if path == 'bad.m3u8':
                    partial = pathlib.Path(current['target_directory']) / f'.{current["id"]}.part'
                    assert pathlib.Path(str(partial) + '.segments').is_dir(), \
                        'failed media task discarded its persisted segments'
            print('missing segments and forbidden local URLs: PASS')

            for path, marker in (('drm.m3u8', 'SAMPLE-AES'), ('drm.mpd', 'Widevine')):
                current = wait(create(path, path + '-out'))
                assert current['state'] == 'failed', current
                assert marker in current['error_message'], current['error_message']
            print('DRM systems are identified and refused without pretending support: PASS')

            slow.set()
            task = create('hls/stream.m3u8', 'delete-cache')
            wait(task, ('downloading',))
            time.sleep(.3)
            partial = pathlib.Path(task['target_directory']) / f'.{task["id"]}.part'
            cache_dir = pathlib.Path(str(partial) + '.segments')
            assert cache_dir.is_dir(), f'segment cache missing: {cache_dir}'
            api('/' + task['id'], method='DELETE')
            assert not cache_dir.exists(), 'deleting a paused task left its segment cache'
            slow.clear()
            print('deleting a paused media task removes its segment cache: PASS')

            reset_live()
            current = wait(create('live/stream.m3u8', 'live-limit', {'record': {'duration': 3}}), timeout=40)
            assert current['state'] == 'completed', current
            assert 2.0 <= current['duration_seconds'] <= 4.5, current
            live_output = pathlib.Path(current['target_directory']) / current['file_name']
            assert any(int(stream['nb_read_packets']) > 0 for stream in probe(live_output))
            print('live duration limit saves a finite recording: PASS')

            reset_live()
            task = create('live/stream.m3u8', 'live-finish')
            wait(task, ('downloading',))
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if api('/' + task['id'])['task']['downloaded_bytes'] > 0:
                    break
                time.sleep(.05)
            api('/' + task['id'] + '/finish', {})
            current = wait(task, timeout=15)
            assert current['state'] == 'completed', current
            assert current['duration_seconds'] > 0, current
            print('live finish saves the recording: PASS')

            reset_live()
            current = wait(create('live/stream.m3u8', 'live-size', {'record': {'size': 40000}}), timeout=30)
            assert current['state'] == 'completed', current
            assert current['downloaded_bytes'] >= 40000, current
            print('live size limit saves the recording: PASS')

            current = wait(create('live/gap.m3u8', 'live-gap'), timeout=20)
            assert current['state'] == 'failed', current
            assert current['gap_count'] >= 1, current
            assert current['error_message'], current
            print('live missing segment is reported as a gap and fails clearly: PASS')

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
