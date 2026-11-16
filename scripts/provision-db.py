#!/usr/bin/env python3
"""Provision an isolated MariaDB for local learning, without changing host MySQL."""
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time

root = Path(os.environ.get('VOD_LOCAL_DB_DIR', Path(__file__).resolve().parent.parent / 'runtime' / 'mysql')).resolve()
base = Path(os.environ['VOD_MARIADB_BASEDIR']).resolve()
port = int(os.environ.get('VOD_LOCAL_DB_PORT', '23306'))
if not 1 <= port <= 65535: raise SystemExit('Invalid database port')
server, install, client = base / 'sbin/mariadbd', base / 'bin/mariadb-install-db', base / 'bin/mariadb'
if not all(p.is_file() and os.access(p, os.X_OK) for p in (server, install, client)):
    raise SystemExit('MariaDB binaries not found in VOD_MARIADB_BASEDIR')
root.mkdir(parents=True, exist_ok=True, mode=0o700)
os.chmod(root, 0o700)
data, credentials, sock, pid, log = root / 'data', root / 'credentials.env', root / 'mysql.sock', root / 'mysql.pid', root / 'mysql.log'
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(base / 'lib/x86_64-linux-gnu') + ':' + env.get('LD_LIBRARY_PATH', '')
if credentials.exists() or data.exists():
    raise SystemExit('Database directory already exists; refusing to overwrite it.')
with socket.socket() as probe:
    if probe.connect_ex(('127.0.0.1', port)) == 0: raise SystemExit('Database port is already in use.')
password = secrets.token_hex(24)
cmd = [str(install), '--no-defaults', '--basedir=' + str(base), '--datadir=' + str(data),
       '--auth-root-authentication-method=normal', '--skip-test-db']
result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, text=True, timeout=90)
if result.returncode: raise SystemExit('Database initialization failed: ' + result.stderr[-2000:])
with log.open('ab') as output:
    process = subprocess.Popen([str(server), '--no-defaults', '--basedir=' + str(base), '--datadir=' + str(data),
        '--socket=' + str(sock), '--pid-file=' + str(pid), '--skip-networking', '--skip-log-bin'],
        stdout=output, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    try:
        deadline = time.monotonic() + 30
        while not sock.exists() and time.monotonic() < deadline:
            if process.poll() is not None: raise RuntimeError('Database exited; inspect mysql.log')
            time.sleep(.2)
        if not sock.exists(): raise RuntimeError('Database startup timed out')
        sql = ("CREATE DATABASE vod_local CHARACTER SET utf8mb4;"
               "CREATE USER 'vod_local'@'localhost' IDENTIFIED BY '" + password + "';"
               "GRANT ALL PRIVILEGES ON vod_local.* TO 'vod_local'@'localhost';"
               "ALTER USER 'root'@'localhost' IDENTIFIED BY '" + secrets.token_hex(32) + "';")
        subprocess.run([str(client), '--no-defaults', '--protocol=socket', '--socket=' + str(sock), '-u', 'root'],
                       input=sql, text=True, check=True, env=env, timeout=20)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        fd = os.open(credentials, flags, 0o600)
        with os.fdopen(fd, 'w') as output_file:
            output_file.write('VOD_DB_HOST=127.0.0.1\nVOD_DB_PORT=' + str(port) + '\nVOD_DB_USER=vod_local\nVOD_DB_NAME=vod_local\nVOD_DB_PASSWORD=' + password + '\n')
        (root / 'basedir.txt').write_text(str(base) + '\n')
    finally:
        process.terminate()
        try: process.wait(timeout=20)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
print('Isolated database provisioned; credentials saved in a private file.')
