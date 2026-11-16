import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

etcd, test, rpc_server, rpc_client = sys.argv[1:]
def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

client_port, peer_port = free_port(), free_port()
while peer_port == client_port:
    peer_port = free_port()
client = f'http://127.0.0.1:{client_port}'
peer = f'http://127.0.0.1:{peer_port}'
with tempfile.TemporaryDirectory(prefix='vod-etcd-') as directory:
    command = [etcd, '--name', 'vod-test', '--data-dir', directory,
               '--listen-client-urls', client, '--advertise-client-urls', client,
               '--listen-peer-urls', peer, '--initial-advertise-peer-urls', peer,
               '--initial-cluster', f'vod-test={peer}', '--initial-cluster-state', 'new']
    log = Path(directory) / 'etcd.log'
    with log.open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('etcd exited before accepting connections')
                try:
                    with socket.create_connection(('127.0.0.1', client_port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                raise RuntimeError('etcd did not start')
            subprocess.run([test, client], check=True, timeout=60)
            rpc_port = free_port()
            rpc_log = Path(directory) / 'rpc.log'
            with rpc_log.open('w') as rpc_output:
                server = subprocess.Popen([rpc_server, '--etcd_address=' + client,
                    '--service_name=test', '--instance_id=one',
                    '--listen_address=127.0.0.1:' + str(rpc_port)],
                    stdout=rpc_output, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 20
                    while time.monotonic() < deadline:
                        if server.poll() is not None:
                            raise RuntimeError('RPC server exited during startup')
                        rpc_output.flush()
                        if 'RPC_READY' in rpc_log.read_text(errors='replace'):
                            break
                        time.sleep(0.1)
                    else:
                        raise RuntimeError('RPC server did not register')
                    result = subprocess.run([rpc_client, '--etcd_address=' + client,
                        '--service_name=test', '--nonce=abc'], capture_output=True, text=True, timeout=20)
                    if result.returncode or 'RPC_HEALTH_OK test' not in result.stdout:
                        raise RuntimeError('RPC call failed: ' + result.stdout + result.stderr)
                finally:
                    server.terminate()
                    try:
                        server.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
                result = subprocess.run([rpc_client, '--etcd_address=' + client,
                    '--service_name=test'], capture_output=True, text=True, timeout=20)
                if result.returncode == 0:
                    raise RuntimeError('RPC client unexpectedly succeeded after service shutdown')
        except Exception:
            output.flush()
            print(log.read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    subprocess.run([test, client, 'offline'], check=True, timeout=20)
