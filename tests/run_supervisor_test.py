import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

script = Path(sys.argv[1]).resolve()
names = ['vod_user_server', 'vod_file_server', 'vod_video_server', 'vod_transcode_worker', 'vod_gateway']
fake = '''#!/usr/bin/env python3
import json, os, signal, sys, time
with open(os.environ['FAKE_RECORD'], 'a') as out:
    out.write(json.dumps({'name': os.path.basename(sys.argv[0]), 'pid': os.getpid(), 'args': sys.argv[1:]}) + '\\n')
    out.flush()
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
while True: time.sleep(1)
'''
with tempfile.TemporaryDirectory(prefix='vod-supervisor-test-') as temp:
    root = Path(temp)
    binaries = root / 'bin'; binaries.mkdir()
    for name in names:
        executable = binaries / name; executable.write_text(fake); executable.chmod(0o755)
    for crash in (False, True):
        record = root / ('crash.jsonl' if crash else 'stop.jsonl')
        env = os.environ.copy()
        env.update(VOD_BIN_DIR=str(binaries), VOD_LOG_DIR=str(root / 'logs'), VOD_DB_USER='test-user',
                   VOD_DB_NAME='test-db', VOD_FDFS_TRACKER='127.0.0.1:12345', FAKE_RECORD=str(record))
        process = subprocess.Popen(['bash', str(script)], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                lines = record.read_text().splitlines() if record.exists() else []
                if len(lines) == 5: break
                if process.poll() is not None: raise RuntimeError('supervisor exited before launching all services')
                time.sleep(.05)
            else: raise RuntimeError('services did not start')
            entries = [json.loads(line) for line in lines]
            assert {entry['name'] for entry in entries} == set(names)
            gateway = next(entry for entry in entries if entry['name'] == 'vod_gateway')
            assert '--http_host=127.0.0.1' in gateway['args']
            if crash: os.kill(entries[0]['pid'], signal.SIGTERM)
            else: process.send_signal(signal.SIGTERM)
            process.communicate(timeout=10)
            assert process.returncode == (1 if crash else 143)
            for entry in entries:
                try: os.kill(entry['pid'], 0)
                except ProcessLookupError: continue
                raise RuntimeError('child process remained after supervisor exit')
        finally:
            if process.poll() is None:
                process.kill(); process.communicate(timeout=5)
