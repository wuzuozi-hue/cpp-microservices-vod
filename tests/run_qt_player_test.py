import os
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='vod-qt-player-') as tmp:
    video = Path(tmp) / 'sample.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc=size=160x90:rate=12', '-t', '1', '-c:v', 'mpeg4', '-threads', '1', str(video)], check=True, timeout=30)
    env = os.environ.copy(); env['QT_QPA_PLATFORM'] = 'offscreen'
    subprocess.run([sys.argv[1], '--self-test-play', str(video)], env=env, check=True, timeout=20)
    print('Qt and mpv playback test passed')
