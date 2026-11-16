from pathlib import Path
import json
import subprocess
import sys
import tempfile

binary = sys.argv[1]
def run(args, ok=True):
    result = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if (result.returncode == 0) != ok:
        raise RuntimeError('unexpected exit: ' + str(args[0]) + '\n' + result.stderr)
    return result.stdout

with tempfile.TemporaryDirectory(prefix='vod-hls-') as directory:
    root = Path(directory)
    for audio in (True, False):
        source = root / ('av.mp4' if audio else 'video.mp4')
        output = root / ('hls-av' if audio else 'hls-video')
        output.mkdir()
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                   '-i', 'testsrc=size=160x90:rate=12']
        if audio: command += ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000']
        command += ['-t', '4', '-c:v', 'mpeg4', '-threads', '1']
        if audio: command += ['-c:a', 'aac']
        run(command + [str(source)])
        run([binary, str(source), str(output)])
        playlist = output / 'index.m3u8'
        content = playlist.read_text()
        assert '#EXT-X-ENDLIST' in content
        segments = [x for x in content.splitlines() if x and not x.startswith('#')]
        assert len(segments) >= 2 and all((output / item).stat().st_size for item in segments)
        info = json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(playlist)]))
        streams = {stream['codec_type']: stream['codec_name'] for stream in info['streams']}
        assert streams == ({'video': 'h264', 'audio': 'aac'} if audio else {'video': 'h264'})
        assert 3.8 < float(info['format']['duration']) < 4.5
        run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-threads', '1', '-i', str(playlist), '-f', 'null', '-'])
        run([binary, str(source), str(output)], False)
    empty = root / 'empty'; empty.mkdir()
    run([binary, str(root / 'missing'), str(empty)], False)
    print('HLS transcode tests passed')
