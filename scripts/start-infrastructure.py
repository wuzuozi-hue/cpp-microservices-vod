#!/usr/bin/env python3
"""Start isolated persistent etcd and FastDFS for the local learning setup."""
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

project = Path(__file__).resolve().parent.parent
root = Path(os.environ.get('VOD_LOCAL_INFRA_DIR', project / 'runtime' / 'infrastructure')).resolve()
etcd = Path(os.environ.get('VOD_LOCAL_ETCD', project / 'runtime/dependencies/etcd/bin/etcd')).resolve()
if not etcd.is_file(): raise SystemExit('Local etcd binary not found.')
root.mkdir(parents=True, exist_ok=True, mode=0o700); os.chmod(root, 0o700)
addresses = subprocess.check_output(['hostname', '-I'], text=True).split()
address = next((a for a in addresses if ipaddress.ip_address(a).version == 4 and ipaddress.ip_address(a).is_private and not ipaddress.ip_address(a).is_loopback), None)
if not address: raise SystemExit('No private container IPv4 address available.')
record = root / 'processes.json'
ports = [('127.0.0.1', 22379), ('127.0.0.1', 22380), (address, 22301), (address, 22302)]
def listening(host, port):
    try:
        with socket.create_connection((host, port), .3): return True
    except OSError: return False
if record.exists():
    saved = json.loads(record.read_text())
    alive = []
    for item in saved.get('processes', []):
        try:
            data = Path('/proc', str(item['pid']), 'cmdline').read_bytes().replace(b'\0', b' ')
            alive.append(str(root).encode() in data and item['binary'].encode() in data)
        except OSError: alive.append(False)
    if all(alive) and len(alive) == 3 and saved.get('address') == address and all(listening(*p) for p in ports):
        print('Local etcd and FastDFS are already running.'); raise SystemExit(0)
    if any(alive): raise SystemExit('Only part of the local infrastructure is running; inspect the recorded processes before retrying.')
for endpoint in ports:
    if listening(*endpoint): raise SystemExit('A required local infrastructure port is in use; no changes made.')
tracker_dir, storage_dir = root / 'tracker', root / 'storage'
tracker_dir.mkdir(exist_ok=True); storage_dir.mkdir(exist_ok=True)
def config(source, destination, updates, remove_trackers=False):
    text = Path(source).read_text()
    if remove_trackers: text = re.sub(r'(?m)^\s*tracker_server\s*=.*\n', '', text)
    for key, value in updates.items():
        pattern = r'(?m)^\s*' + re.escape(key) + r'\s*=.*$'
        if re.search(pattern, text): text = re.sub(pattern, lambda _: key + ' = ' + str(value), text)
        else: text += '\n' + key + ' = ' + str(value) + '\n'
    destination.write_text(text)
tracker_conf, storage_conf, client_conf = root / 'tracker.conf', root / 'storage.conf', root / 'client.conf'
config('/etc/fdfs/tracker.conf', tracker_conf, {'bind_addr': address, 'port': 22301, 'base_path': tracker_dir, 'reserved_storage_space': '1MB', 'http.server_port': 22303})
config('/etc/fdfs/storage.conf', storage_conf, {'bind_addr': address, 'port': 22302, 'base_path': storage_dir, 'store_path0': storage_dir,
    'tracker_server': f'{address}:22301', 'subdir_count_per_path': 4, 'heart_beat_interval': 1, 'stat_report_interval': 1, 'http.server_port': 22304}, True)
client_conf.write_text(f'base_path={root}\ntracker_server={address}:22301\nconnect_timeout=2\nnetwork_timeout=3\n')
commands = [
    [str(etcd), '--name', 'vod-local', '--data-dir', str(root / 'etcd-data'), '--listen-client-urls', 'http://127.0.0.1:22379',
     '--advertise-client-urls', 'http://127.0.0.1:22379', '--listen-peer-urls', 'http://127.0.0.1:22380',
     '--initial-advertise-peer-urls', 'http://127.0.0.1:22380', '--initial-cluster', 'vod-local=http://127.0.0.1:22380', '--initial-cluster-state', 'new'],
    ['/usr/bin/fdfs_trackerd', '-N', str(tracker_conf), 'start'], ['/usr/bin/fdfs_storaged', '-N', str(storage_conf), 'start']]
children = []
try:
    for index, command in enumerate(commands):
        with (root / ('etcd.log', 'tracker.log', 'storage.log')[index]).open('ab') as output:
            child = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        children.append(child)
        endpoint = ports[(0, 2, 3)[index]]; deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if child.poll() is not None: raise RuntimeError('Infrastructure process exited; inspect logs.')
            if listening(*endpoint): break
            time.sleep(.2)
        else: raise RuntimeError('Infrastructure startup timed out.')
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if any(p.poll() is not None for p in children): raise RuntimeError('Infrastructure process exited; inspect logs.')
        result = subprocess.run(['/usr/bin/fdfs_monitor', str(client_conf)], capture_output=True, text=True, timeout=6)
        if result.returncode == 0 and 'ACTIVE' in result.stdout: break
        time.sleep(1)
    else: raise RuntimeError('FastDFS storage did not become active.')
    record.write_text(json.dumps({'address': address, 'processes': [{'pid': p.pid, 'binary': Path(c[0]).name} for p, c in zip(children, commands)]}))
    (root / 'endpoints.env').write_text('VOD_ETCD_ADDRESS=http://127.0.0.1:22379\nVOD_FDFS_TRACKER=' + address + ':22301\n')
    print('Local etcd and FastDFS started; storage is active.')
except Exception:
    for child in reversed(children):
        if child.poll() is None: child.terminate()
    for child in reversed(children):
        try: child.wait(timeout=15)
        except subprocess.TimeoutExpired: child.kill(); child.wait()
    raise
