#!/usr/bin/env python3
"""Start the project's already-provisioned private database."""
import os
from pathlib import Path
import socket
import subprocess
import time

root = Path(os.environ.get('VOD_LOCAL_DB_DIR', Path(__file__).resolve().parent.parent / 'runtime' / 'mysql')).resolve()
credentials = root / 'credentials.env'
if not credentials.is_file() or not (root / 'basedir.txt').is_file(): raise SystemExit('Run provision-db.py first.')
values = dict(line.split('=', 1) for line in credentials.read_text().splitlines())
port = int(values['VOD_DB_PORT']); base = Path((root / 'basedir.txt').read_text().strip())
server = base / 'sbin/mariadbd'; pidfile = root / 'mysql.pid'
def listening():
    try:
        with socket.create_connection(('127.0.0.1', port), .3): return True
    except OSError: return False
if pidfile.exists():
    try:
        pid = int(pidfile.read_text().strip())
        command = Path('/proc') .joinpath(str(pid), 'cmdline').read_bytes().replace(b'\0', b' ')
        if str(root / 'data').encode() in command and b'mariadbd' in command and listening():
            print('Isolated database is already running.'); raise SystemExit(0)
    except (OSError, ValueError): pass
if listening(): raise SystemExit('Configured port is in use by another process; no changes made.')
env = os.environ.copy(); env['LD_LIBRARY_PATH'] = str(base / 'lib/x86_64-linux-gnu') + ':' + env.get('LD_LIBRARY_PATH', '')
with (root / 'mysql.log').open('ab') as output:
    process = subprocess.Popen([str(server), '--no-defaults', '--basedir=' + str(base), '--datadir=' + str(root / 'data'),
        '--socket=' + str(root / 'mysql.sock'), '--pid-file=' + str(pidfile), '--bind-address=127.0.0.1',
        '--port=' + str(port), '--skip-log-bin'], stdout=output, stderr=subprocess.STDOUT, env=env, start_new_session=True)
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    if process.poll() is not None: raise SystemExit('Database exited; inspect mysql.log.')
    if listening(): print('Isolated database started on 127.0.0.1:' + str(port)); break
    time.sleep(.2)
else:
    process.terminate(); process.wait(timeout=20); raise SystemExit('Database startup timed out.')
