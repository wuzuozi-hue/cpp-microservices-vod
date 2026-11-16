from contextlib import contextmanager
from pathlib import Path
import ipaddress
import re
import socket
import subprocess
import sys
import tempfile
import time

@contextmanager
def isolated_fdfs(tracker, storage, monitor):
    addresses = subprocess.check_output(['hostname', '-I'], text=True).split()
    address = next((value for value in addresses if ipaddress.ip_address(value).version == 4 and
                    ipaddress.ip_address(value).is_private and not ipaddress.ip_address(value).is_loopback), None)
    if not address:
        raise RuntimeError('a private, non-loopback container IPv4 address is required for FastDFS')
    def free_port():
        with socket.socket() as sock:
            sock.bind((address, 0))
            return sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='vod-fdfs-') as tmp:
        root = Path(tmp)
        tracker_dir, storage_dir = root / 'tracker', root / 'storage'
        tracker_dir.mkdir(); storage_dir.mkdir()
        tracker_port, storage_port = free_port(), free_port()
        while tracker_port == storage_port:
            storage_port = free_port()
        def config(source, path, updates, remove_trackers=False):
            text = Path(source).read_text()
            if remove_trackers:
                text = re.sub(r'(?m)^\s*tracker_server\s*=.*\n', '', text)
            for key, value in updates.items():
                pattern = r'(?m)^\s*' + re.escape(key) + r'\s*=.*$'
                if re.search(pattern, text):
                    text = re.sub(pattern, key + ' = ' + str(value), text)
                else:
                    text += '\n' + key + ' = ' + str(value) + '\n'
            path.write_text(text)
        tracker_conf, storage_conf, client_conf = root / 'tracker.conf', root / 'storage.conf', root / 'client.conf'
        config('/etc/fdfs/tracker.conf', tracker_conf, {'bind_addr': address, 'port': tracker_port,
            'base_path': tracker_dir, 'reserved_storage_space': '1MB', 'http.server_port': free_port()})
        config('/etc/fdfs/storage.conf', storage_conf, {'bind_addr': address, 'port': storage_port,
            'base_path': storage_dir, 'store_path0': storage_dir, 'tracker_server': f'{address}:{tracker_port}',
            'subdir_count_per_path': 4, 'heart_beat_interval': 1, 'stat_report_interval': 1,
            'http.server_port': free_port()}, True)
        client_conf.write_text(f'base_path={root}\ntracker_server={address}:{tracker_port}\nconnect_timeout=2\nnetwork_timeout=3\n')
        children = []
        with (root / 'process.log').open('w') as log:
            try:
                for binary, cfg in ((tracker, tracker_conf), (storage, storage_conf)):
                    child = subprocess.Popen([binary, '-N', str(cfg), 'start'], stdout=log, stderr=subprocess.STDOUT)
                    children.append(child)
                    if binary == tracker:
                        deadline = time.monotonic() + 15
                        while time.monotonic() < deadline:
                            if child.poll() is not None: raise RuntimeError('tracker exited')
                            try:
                                with socket.create_connection((address, tracker_port), timeout=.2): break
                            except OSError: time.sleep(.1)
                        else: raise RuntimeError('tracker did not start')
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    if any(child.poll() is not None for child in children): raise RuntimeError('FastDFS service exited')
                    result = subprocess.run([monitor, str(client_conf)], capture_output=True, text=True, timeout=6)
                    if result.returncode == 0 and 'ACTIVE' in result.stdout: break
                    time.sleep(1)
                else: raise RuntimeError('storage did not register as active')
                yield f'{address}:{tracker_port}'
            except Exception:
                log.flush()
                print((root / 'process.log').read_text(errors='replace'), file=sys.stderr)
                for path in root.rglob('*.log'):
                    if path != root / 'process.log': print(path.read_text(errors='replace')[-4000:], file=sys.stderr)
                raise
            finally:
                for child in reversed(children):
                    if child.poll() is None:
                        child.terminate()
                        try: child.wait(timeout=10)
                        except subprocess.TimeoutExpired: child.kill(); child.wait()
