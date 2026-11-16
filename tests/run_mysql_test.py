import os
import json
import re
import secrets
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from fake_smtp import Server as SmtpServer
from fdfs_fixture import isolated_fdfs

base, test, schema, etcd, user_server, gateway, video_test, video_schema, verification_test, account_schema, file_schema, tracker, storage, monitor, file_server, video_server, job_test, worker, mq_cleanup, account_test = map(Path, sys.argv[1:])
server_binary = base / 'sbin/mariadbd'
install = base / 'bin/mariadb-install-db'
client = base / 'bin/mariadb'

with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
with tempfile.TemporaryDirectory(prefix='vod-mysql-') as folder:
    root = Path(folder)
    data = root / 'data'
    env = os.environ.copy()
    libraries = str(base / 'lib/x86_64-linux-gnu')
    env['LD_LIBRARY_PATH'] = libraries + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    init = subprocess.run([str(install), '--no-defaults', '--basedir=' + str(base),
        '--datadir=' + str(data), '--auth-root-authentication-method=normal', '--skip-test-db'],
        capture_output=True, text=True, env=env, timeout=60)
    if init.returncode:
        raise RuntimeError('temporary database initialization failed: ' + init.stderr)
    socket_path = root / 'mysql.sock'
    logfile = root / 'server.log'
    command = [str(server_binary), '--no-defaults', '--basedir=' + str(base), '--datadir=' + str(data),
        '--socket=' + str(socket_path), '--pid-file=' + str(root / 'server.pid'),
        '--bind-address=127.0.0.1', '--port=' + str(port), '--skip-log-bin']
    with logfile.open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, env=env)
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('temporary database exited')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                raise RuntimeError('temporary database did not start')
            subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1',
                '-P', str(port), '-u', 'root'], input='CREATE DATABASE vod_test;', text=True, check=True, env=env, timeout=20)
            init_env = dict(env, VOD_DB_HOST='127.0.0.1', VOD_DB_PORT=str(port), VOD_DB_USER='root',
                            VOD_DB_NAME='vod_test', VOD_DB_PASSWORD='', VOD_MYSQL_CLIENT=str(client))
            init_script = schema.parent.parent / 'scripts' / 'init-db.sh'
            for _ in range(2): subprocess.run(['bash', str(init_script)], check=True, env=init_env, timeout=20)
            env.update(VOD_TEST_DB_HOST='127.0.0.1', VOD_TEST_DB_PORT=str(port),
                VOD_TEST_DB_USER='root', VOD_TEST_DB_PASSWORD='', VOD_TEST_DB_NAME='vod_test')
            subprocess.run([str(test)], check=True, env=env, timeout=30)
            subprocess.run([str(video_test)], check=True, env=env, timeout=30)
            subprocess.run([str(job_test)], check=True, env=env, timeout=30)
            subprocess.run([str(verification_test)], check=True, env=env, timeout=30)
            subprocess.run([str(account_test)], check=True, env=env, timeout=60)
            def free_port():
                with socket.socket() as port_sock:
                    port_sock.bind(('127.0.0.1', 0))
                    return port_sock.getsockname()[1]
            etcd_port, peer_port, user_port, http_port = [free_port() for _ in range(4)]
            if len({etcd_port, peer_port, user_port, http_port, port}) != 5:
                raise RuntimeError('test ports collided; retry')
            etcd_url = f'http://127.0.0.1:{etcd_port}'
            peer_url = f'http://127.0.0.1:{peer_port}'
            processes = []
            mq_namespace = ''
            if env.get('VOD_TEST_AMQP_URL'):
                mq_namespace = 'vod.test.' + secrets.token_hex(12)
                env.update(VOD_AMQP_URL=env['VOD_TEST_AMQP_URL'], VOD_MQ_NAMESPACE=mq_namespace)
            certificate, key = root / 'smtp-cert.pem', root / 'smtp-key.pem'
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-keyout', str(key), '-out', str(certificate), '-days', '1', '-subj', '/CN=localhost',
                '-addext', 'subjectAltName=DNS:localhost'], capture_output=True, check=True, timeout=20)
            smtp = SmtpServer(certificate, key)
            with (root / 'e2e.log').open('w') as e2e_log:
                def start(command):
                    child = subprocess.Popen(command, stdout=e2e_log, stderr=subprocess.STDOUT, env=env)
                    processes.append(child)
                    return child
                def wait_port(port_number, process):
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError('service exited during startup')
                        try:
                            with socket.create_connection(('127.0.0.1', port_number), timeout=.2):
                                return
                        except OSError:
                            time.sleep(.1)
                    raise RuntimeError('service startup timed out')
                def post(path, obj):
                    req = urllib.request.Request(f'http://127.0.0.1:{http_port}' + path,
                        data=json.dumps(obj).encode(), headers={'Content-Type': 'application/json'})
                    try:
                        with urllib.request.urlopen(req, timeout=15) as response:
                            return response.status, json.load(response)
                    except urllib.error.HTTPError as error:
                        return error.code, json.load(error)
                try:
                    etcd_process = start([str(etcd), '--name', 'vod-session-test', '--data-dir', str(root / 'etcd'),
                        '--listen-client-urls', etcd_url, '--advertise-client-urls', etcd_url,
                        '--listen-peer-urls', peer_url, '--initial-advertise-peer-urls', peer_url,
                        '--initial-cluster', 'vod-session-test=' + peer_url, '--initial-cluster-state', 'new'])
                    wait_port(etcd_port, etcd_process)
                    env['VOD_DB_PASSWORD'] = ''
                    env.update(VOD_CODE_SECRET='test-secret-for-isolated-system-only',
                        VOD_SMTP_URL='smtp://localhost:' + str(smtp.server_address[1]),
                        VOD_SMTP_FROM='noreply@example.test', VOD_SMTP_CA_FILE=str(certificate),
                        VOD_SMTP_USER='test-user', VOD_SMTP_PASSWORD='test-password', VOD_SMTP_LOGIN_OPTIONS='AUTH=LOGIN')
                    user = start([str(user_server), '--etcd_address=' + etcd_url, '--instance_id=test',
                        '--listen_address=127.0.0.1:' + str(user_port), '--db_host=127.0.0.1',
                        '--db_port=' + str(port), '--db_user=root', '--db_name=vod_test'])
                    deadline = time.monotonic() + 30
                    while time.monotonic() < deadline:
                        if user.poll() is not None:
                            raise RuntimeError('user service exited during registration')
                        e2e_log.flush()
                        if 'USER_READY' in (root / 'e2e.log').read_text(errors='replace'):
                            break
                        time.sleep(.1)
                    else:
                        raise RuntimeError('user service registration timed out')
                    http = start([str(gateway), '--etcd_address=' + etcd_url, '--http_port=' + str(http_port)])
                    wait_port(http_port, http)
                    status, response = post('/HttpService/tempLogin', {'requestId': 'test-1'})
                    if status != 200 or response.get('errorCode') != 0 or response.get('requestId') != 'test-1':
                        raise RuntimeError('guest login failed: ' + str(response))
                    token = response['result']['sessionId']
                    if len(token) != 64:
                        raise RuntimeError('invalid guest token')
                    status, response = post('/HttpService/sessionLogin', {'requestId': 'test-2', 'sessionId': token})
                    if status != 200 or response.get('errorCode') != 0 or response['result'].get('isGuest') is not True:
                        raise RuntimeError('session login failed: ' + str(response))
                    email = 'e2e-user@example.test'
                    status, response = post('/HttpService/getCode', {'requestId': 'test-code', 'sessionId': token, 'email': email})
                    if status != 200 or response.get('errorCode') != 0:
                        raise RuntimeError('email verification request failed: ' + str(response))
                    code_id = response['result']['codeId']
                    mail = smtp.messages.get(timeout=5).decode('utf-8')
                    match = re.search(r'Verification code: ([0-9]{6})', mail)
                    if not match:
                        raise RuntimeError('verification message not received')
                    code = match.group(1)
                    login = {'requestId': 'test-code-login', 'sessionId': token, 'email': email,
                             'codeId': code_id, 'verifyCode': code}
                    status, response = post('/HttpService/vcodeLogin', login)
                    if status != 200 or response.get('errorCode') != 0:
                        raise RuntimeError('email login failed: ' + str(response))
                    status, response = post('/HttpService/vcodeLogin', login)
                    if status != 200 or response.get('errorCode') != 1002:
                        raise RuntimeError('verification code replay accepted')
                    status, response = post('/HttpService/sessionLogin', {'requestId': 'test-account', 'sessionId': token})
                    if status != 200 or response.get('errorCode') != 0 or response['result'].get('isGuest') is not False:
                        raise RuntimeError('account session login failed')
                    status, response = post('/HttpService/setNickname', {'requestId': 'nickname', 'sessionId': token, 'nickname': 'video-student'})
                    if status != 200 or response.get('errorCode') != 0: raise RuntimeError('nickname update failed')
                    status, response = post('/HttpService/setPassword', {'requestId': 'password', 'sessionId': token, 'password': 'practicePassword9!'})
                    if status != 200 or response.get('errorCode') != 0: raise RuntimeError('password update failed')
                    status, response = post('/HttpService/getUserInfo', {'requestId': 'profile', 'sessionId': token})
                    if status != 200 or response.get('errorCode') != 0 or response['result']['userInfo']['nickname'] != 'video-student':
                        raise RuntimeError('profile lookup failed')
                    status, response = post('/HttpService/tempLogin', {'requestId': 'password-guest'})
                    password_token = response['result']['sessionId']
                    status, response = post('/HttpService/passwdLogin', {'requestId': 'bad-password', 'sessionId': password_token,
                        'username': 'video-student', 'password': 'incorrect'})
                    if status != 200 or response.get('errorCode') != 1002: raise RuntimeError('wrong password accepted')
                    status, response = post('/HttpService/passwdLogin', {'requestId': 'good-password', 'sessionId': password_token,
                        'username': 'video-student', 'password': 'practicePassword9!'})
                    if status != 200 or response.get('errorCode') != 0: raise RuntimeError('password login failed')
                    status, response = post('/HttpService/sessionLogin', {'requestId': 'password-session', 'sessionId': password_token})
                    if status != 200 or response.get('errorCode') != 0 or response['result'].get('isGuest') is not False:
                        raise RuntimeError('password session not associated with user')
                    if str(tracker) != '-':
                        with isolated_fdfs(str(tracker), str(storage), str(monitor)) as tracker_address:
                            file_port = free_port()
                            file_process = start([str(file_server), '--etcd_address=' + etcd_url, '--instance_id=test',
                                '--http_port=' + str(file_port), '--tracker=' + tracker_address, '--db_host=127.0.0.1',
                                '--db_port=' + str(port), '--db_user=root', '--db_name=vod_test'])
                            deadline = time.monotonic() + 30
                            while time.monotonic() < deadline:
                                if file_process.poll() is not None: raise RuntimeError('file service exited')
                                e2e_log.flush()
                                if 'FILE_READY' in (root / 'e2e.log').read_text(errors='replace'): break
                                time.sleep(.1)
                            else: raise RuntimeError('file service registration timed out')
                            video_port = free_port()
                            video_process = start([str(video_server), '--etcd_address=' + etcd_url, '--instance_id=test',
                                '--listen_address=127.0.0.1:' + str(video_port), '--db_host=127.0.0.1',
                                '--db_port=' + str(port), '--db_user=root', '--db_name=vod_test'])
                            deadline = time.monotonic() + 30
                            while time.monotonic() < deadline:
                                if video_process.poll() is not None: raise RuntimeError('video service exited')
                                e2e_log.flush()
                                if 'VIDEO_READY' in (root / 'e2e.log').read_text(errors='replace'): break
                                time.sleep(.1)
                            else: raise RuntimeError('video service registration timed out')
                            worker_process = None
                            if mq_namespace:
                                worker_process = start([str(worker), '--tracker=' + tracker_address, '--poll_seconds=30',
                                    '--db_host=127.0.0.1', '--db_port=' + str(port), '--db_user=root', '--db_name=vod_test'])
                                deadline = time.monotonic() + 30
                                while time.monotonic() < deadline:
                                    if worker_process.poll() is not None: raise RuntimeError('MQ worker exited during startup')
                                    e2e_log.flush()
                                    log_text = (root / 'e2e.log').read_text(errors='replace')
                                    if 'MQ_WAKE_ENABLED' in log_text and 'WORKER_IDLE' in log_text: break
                                    time.sleep(.1)
                                else: raise RuntimeError('MQ worker did not enter idle state')
                            def binary_request(method, path, params, payload=None):
                                url = f'http://127.0.0.1:{http_port}' + path + '?' + urllib.parse.urlencode(params)
                                request = urllib.request.Request(url, data=payload, method=method,
                                    headers={'Content-Type': 'application/octet-stream'} if payload is not None else {})
                                try:
                                    with urllib.request.urlopen(request, timeout=30) as result:
                                        return result.status, result.read()
                                except urllib.error.HTTPError as error:
                                    return error.code, error.read()
                            guest_status, guest_response = post('/HttpService/tempLogin', {'requestId': 'other-guest'})
                            if guest_status != 200: raise RuntimeError('second guest login failed')
                            guest_token = guest_response['result']['sessionId']
                            data = b'video-test\x00binary\xff\n'
                            status, _ = binary_request('POST', '/HttpService/uploadVideo',
                                {'requestId': 'guest-upload', 'sessionId': guest_token}, data)
                            if status != 401: raise RuntimeError('guest upload was not rejected')
                            status, raw = binary_request('POST', '/HttpService/uploadVideo',
                                {'requestId': 'owner-upload', 'sessionId': token}, data)
                            uploaded = json.loads(raw)
                            if status != 200 or uploaded.get('errorCode') != 0: raise RuntimeError('file upload failed: ' + str(uploaded))
                            file_id = uploaded['result']['fileId']
                            status, result = binary_request('GET', '/HttpService/downloadVideo',
                                {'requestId': 'owner-download', 'sessionId': token, 'fileId': file_id})
                            if status != 200 or result != data: raise RuntimeError('file roundtrip mismatch')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': file_id})
                            if status != 404: raise RuntimeError('anonymous access to private file was allowed')
                            media = root / 'input.mp4'
                            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi',
                                '-i', 'testsrc=size=160x90:rate=12', '-t', '4', '-c:v', 'mpeg4', '-threads', '1', str(media)],
                                check=True, timeout=30)
                            status, raw = binary_request('POST', '/HttpService/uploadVideo',
                                {'requestId': 'media-upload', 'sessionId': token}, media.read_bytes())
                            if status != 200: raise RuntimeError('video input upload HTTP status: ' + str(status))
                            uploaded_media = json.loads(raw)
                            if status != 200 or uploaded_media.get('errorCode') != 0: raise RuntimeError('video input upload failed')
                            video_info = {'videoFileId': uploaded_media['result']['fileId'], 'videoTitle': 'A test video', 'videoDesc': 'description', 'videoType': 4, 'duration': 4, 'videoTag': [7, 8, 7]}
                            status, response = post('/HttpService/newVideo', {'requestId': 'source-as-cover', 'sessionId': token,
                                'videoInfo': dict(video_info, photoFileId=video_info['videoFileId'])})
                            if response.get('errorCode') != 1001: raise RuntimeError('source could be exposed as its own cover')
                            cover_bytes = b'cover image test data'
                            status, raw = binary_request('POST', '/HttpService/uploadPhoto', {'requestId': 'cover-upload', 'sessionId': token}, cover_bytes)
                            cover_response = json.loads(raw)
                            if status != 200 or cover_response.get('errorCode') != 0: raise RuntimeError('cover upload failed')
                            cover_id = cover_response['result']['fileId']; video_info['photoFileId'] = cover_id
                            status, response = post('/HttpService/newVideo', {'requestId': 'new-video', 'sessionId': token, 'videoInfo': video_info})
                            if status != 200 or response.get('errorCode') != 0: raise RuntimeError('video creation failed: ' + str(response))
                            video_id = response['result']['videoId']
                            if not re.fullmatch('[0-9a-f]{64}', video_id): raise RuntimeError('invalid video ID')
                            status, response = post('/HttpService/newVideo', {'requestId': 'other-source-as-cover', 'sessionId': token,
                                'videoInfo': dict(video_info, videoFileId=file_id, photoFileId=video_info['videoFileId'])})
                            if response.get('errorCode') != 1001: raise RuntimeError('another source could be exposed as a cover')
                            query = "SELECT v.status,j.status FROM vod_videos v JOIN vod_transcode_jobs j ON v.video_id=j.video_id WHERE v.video_id='" + video_id + "'"
                            checked = subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-N', '-D', 'vod_test', '-e', query], capture_output=True, text=True, check=True, env=env, timeout=20)
                            observed = checked.stdout.strip()
                            valid_states = {'5\t0', '5\t1', '1\t2'} if worker_process else {'5\t0'}
                            if observed not in valid_states: raise RuntimeError('video and job were not committed in an expected state: ' + observed)
                            if worker_process:
                                deadline = time.monotonic() + 15
                                while time.monotonic() < deadline:
                                    if worker_process.poll() is not None: raise RuntimeError('MQ worker exited during processing')
                                    e2e_log.flush()
                                    if 'JOB_COMPLETE ' + video_id in (root / 'e2e.log').read_text(errors='replace'): break
                                    time.sleep(.1)
                                else: raise RuntimeError('MQ wake did not process job before the 30-second polling interval')
                            else:
                                processed = subprocess.run([str(worker), '--once', '--tracker=' + tracker_address,
                                    '--db_host=127.0.0.1', '--db_port=' + str(port), '--db_user=root', '--db_name=vod_test'],
                                    capture_output=True, text=True, env=env, timeout=120)
                                if processed.returncode or 'JOB_COMPLETE ' + video_id not in processed.stdout:
                                    raise RuntimeError('transcode job failed: ' + processed.stderr)
                            query = "SELECT v.status,j.status,v.playlist_file_id FROM vod_videos v JOIN vod_transcode_jobs j ON v.video_id=j.video_id WHERE v.video_id='" + video_id + "'"
                            checked = subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-N', '-D', 'vod_test', '-e', query], capture_output=True, text=True, check=True, env=env, timeout=20)
                            fields = checked.stdout.strip().split('\t')
                            if len(fields) != 3 or fields[:2] != ['1', '2']: raise RuntimeError('transcode status was not committed')
                            playlist_id = fields[2]
                            status, playlist = binary_request('GET', '/HttpService/downloadVideo',
                                {'requestId': 'playlist', 'sessionId': token, 'fileId': playlist_id})
                            if status != 200: raise RuntimeError('owner could not download playlist')
                            local_hls = root / 'downloaded-hls'; local_hls.mkdir()
                            rewritten = []
                            segment_count = 0
                            for line in playlist.decode().splitlines():
                                if line and not line.startswith('#'):
                                    params = urllib.parse.parse_qs(urllib.parse.urlsplit(line).query)
                                    status, segment = binary_request('GET', '/HttpService/downloadVideo',
                                        {'requestId': 'segment', 'sessionId': token, 'fileId': params['fileId'][0]})
                                    if status != 200: raise RuntimeError('owner could not download HLS segment')
                                    name = f'segment-{segment_count}.ts'
                                    (local_hls / name).write_bytes(segment)
                                    line = name
                                    segment_count += 1
                                rewritten.append(line)
                            if segment_count < 2: raise RuntimeError('too few HLS segments')
                            (local_hls / 'index.m3u8').write_text('\n'.join(rewritten) + '\n')
                            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-threads', '1',
                                '-i', str(local_hls / 'index.m3u8'), '-f', 'null', '-'], check=True, timeout=30)
                            other_email = 'other-e2e@example.test'
                            status, response = post('/HttpService/getCode', {'requestId': 'other-code', 'sessionId': guest_token, 'email': other_email})
                            if status != 200 or response.get('errorCode') != 0: raise RuntimeError('other code request failed')
                            other_id = response['result']['codeId']
                            other_mail = smtp.messages.get(timeout=5).decode('utf-8')
                            other_match = re.search(r'Verification code: ([0-9]{6})', other_mail)
                            if not other_match: raise RuntimeError('other code mail missing')
                            status, response = post('/HttpService/vcodeLogin', {'requestId': 'other-login', 'sessionId': guest_token,
                                'email': other_email, 'codeId': other_id, 'verifyCode': other_match.group(1)})
                            if status != 200 or response.get('errorCode') != 0: raise RuntimeError('other user login failed')
                            avatar_bytes = b'avatar test data'
                            status, raw = binary_request('POST', '/HttpService/uploadPhoto', {'requestId': 'avatar-upload', 'sessionId': token}, avatar_bytes)
                            avatar_id = json.loads(raw)['result']['fileId']
                            status, response = post('/HttpService/setAvatar', {'requestId': 'foreign-avatar', 'sessionId': guest_token, 'fileId': avatar_id})
                            if response.get('errorCode') != 1001: raise RuntimeError('foreign avatar was accepted')
                            status, response = post('/HttpService/setAvatar', {'requestId': 'source-avatar', 'sessionId': token, 'fileId': video_info['videoFileId']})
                            if response.get('errorCode') != 1001: raise RuntimeError('private video source accepted as avatar')
                            status, response = post('/HttpService/setAvatar', {'requestId': 'avatar', 'sessionId': token, 'fileId': avatar_id})
                            if status != 200 or response.get('errorCode') != 0: raise RuntimeError('avatar setting failed')
                            status, avatar = binary_request('GET', '/HttpService/downloadPhoto', {'fileId': avatar_id})
                            if status != 200 or avatar != avatar_bytes: raise RuntimeError('public avatar download failed')
                            status, owner_profile = post('/HttpService/getUserInfo', {'requestId': 'owner-profile', 'sessionId': token})
                            if owner_profile.get('result', {}).get('userInfo', {}).get('userAvatarId') != avatar_id: raise RuntimeError('avatar missing from profile')
                            status, other_profile = post('/HttpService/getUserInfo', {'requestId': 'other-profile', 'sessionId': guest_token})
                            owner_id = owner_profile['result']['userInfo']['userId']; other_id = other_profile['result']['userInfo']['userId']
                            status, response = post('/Httpvice/getUserInfo', {'requestId': 'public-profile', 'sessionId': guest_token, 'userId': owner_id})
                            public_info = response.get('result', {}).get('userInfo', {})
                            if status != 200 or public_info.get('userId') != owner_id or public_info.get('nickname') != 'video-student' or 'email' in public_info:
                                raise RuntimeError('public profile missing or leaked private email')
                            admin_info = {'email': other_email, 'nickname': 'reviewer', 'roleType': 2, 'userStatus': 1, 'userMemo': 'test admin'}
                            admin_request = {'requestId': 'add-admin', 'sessionId': token, 'userInfo': admin_info}
                            status, response = post('/HttpService/newAdministrator', admin_request)
                            if response.get('errorCode') != 1001: raise RuntimeError('ordinary user could grant admin')
                            bootstrap_env = dict(env, VOD_DB_HOST='127.0.0.1', VOD_DB_PORT=str(port), VOD_DB_USER='root',
                                                 VOD_DB_NAME='vod_test', VOD_BOOTSTRAP_EMAIL=email, VOD_MYSQL_CLIENT=str(client), VOD_DB_PASSWORD='')
                            bootstrap = schema.parent.parent / 'scripts' / 'bootstrap-admin.sh'
                            subprocess.run(['bash', str(bootstrap)], check=True, env=bootstrap_env, timeout=20)
                            if subprocess.run(['bash', str(bootstrap)], env=bootstrap_env, capture_output=True, timeout=20).returncode == 0:
                                raise RuntimeError('bootstrap allowed a second super administrator')
                            status, response = post('/HttpService/newAdministrator', admin_request)
                            if response.get('errorCode') != 0: raise RuntimeError('admin grant failed: ' + str(response))
                            status, response = post('/HttpService/getAdminByPhone', {'requestId': 'get-admin', 'sessionId': token, 'email': other_email})
                            if response.get('result', {}).get('userInfo', {}).get('userId') != other_id: raise RuntimeError('admin lookup failed')
                            status, response = post('/HttpService/getAdminListByStatus', {'requestId': 'admin-list', 'sessionId': token, 'pageIndex': 1, 'pageCount': 20, 'userStatus': 1})
                            if response.get('result', {}).get('totalCount') != 1: raise RuntimeError('admin list failed')
                            edit = {'requestId': 'edit-admin', 'sessionId': token,
                                    'userInfo': {'userId': other_id, 'nickname': 'reviewer', 'userStatus': 0, 'userMemo': 'paused'}}
                            status, response = post('/HttpService/setStatus', {'requestId': 'disable-admin', 'sessionId': token, 'userId': other_id, 'userStatus': 0})
                            if response.get('errorCode') != 0: raise RuntimeError('admin disable failed')
                            status, response = post('/HttpService/getAdminByPhone', {'requestId': 'disabled-info', 'sessionId': token, 'email': other_email})
                            admin = response.get('result', {}).get('userInfo', {})
                            if admin.get('userStatus') != 0 or admin.get('nickname') != 'reviewer' or admin.get('userMemo') != 'test admin':
                                raise RuntimeError('admin status change lost account details')
                            status, response = post('/HttpService/sessionLogin', {'requestId': 'disabled-admin-user-session', 'sessionId': guest_token})
                            if response.get('errorCode') != 0 or response.get('result', {}).get('isGuest') is not False:
                                raise RuntimeError('disabled admin lost ordinary user session')
                            edit['userInfo']['userStatus'] = 1
                            status, response = post('/HttpService/setAdministrator', edit)
                            if response.get('errorCode') != 0: raise RuntimeError('admin enable failed')
                            status, response = post('/HttpService/delAdministrator', {'requestId': 'remove-admin', 'sessionId': token, 'userId': other_id})
                            if response.get('errorCode') != 0: raise RuntimeError('admin removal failed')
                            subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port), '-u', 'root',
                                '-D', 'vod_test', '-e', "UPDATE vod_users SET role_type=3 WHERE email='e2e-user@example.test'"], check=True, env=env, timeout=20)
                            follow_request = {'requestId': 'follow', 'sessionId': token, 'userId': other_id}
                            for _ in range(2):
                                status, response = post('/HttpService/newAttention', follow_request)
                                if response.get('errorCode') != 0: raise RuntimeError('follow failed')
                            status, response = post('/HttpService/newAttention', dict(follow_request, userId=owner_id))
                            if response.get('errorCode') != 1001: raise RuntimeError('self follow was accepted')
                            result = subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-N', '-D', 'vod_test', '-e', 'SELECT COUNT(*) FROM vod_follows'], capture_output=True, text=True, check=True, env=env, timeout=20)
                            if result.stdout.strip() != '1': raise RuntimeError('duplicate follow stored')
                            status, response = post('/HttpService/delAttention', follow_request)
                            if response.get('errorCode') != 0: raise RuntimeError('unfollow failed')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo',
                                {'requestId': 'other-download', 'sessionId': guest_token, 'fileId': file_id})
                            if status != 404: raise RuntimeError('another user accessed private file')
                            status, response = post('/HttpService/newVideo', {'requestId': 'other-video', 'sessionId': guest_token, 'videoInfo': video_info})
                            if status != 200 or response.get('errorCode') != 1001: raise RuntimeError('foreign file accepted for video')
                            status, response = post('/HttpService/userVideoList', {'requestId': 'owner-list', 'sessionId': token, 'pageIndex': 1, 'pageCount': 20})
                            if status != 200 or not any(v['videoId'] == video_id and v['videoStatus'] == 1 for v in response.get('result', {}).get('videoList', [])):
                                raise RuntimeError('owner pending video missing')
                            status, response = post('/HttpService/userVideoList', {'requestId': 'other-list', 'sessionId': guest_token, 'pageIndex': 1, 'pageCount': 20})
                            if status != 200 or response.get('result', {}).get('totalCount') != 0: raise RuntimeError('other user saw private video')
                            review = {'requestId': 'review', 'sessionId': token, 'videoId': video_id, 'checkResult': True}
                            status, response = post('/HttpService/checkVideo', review)
                            if status != 403: raise RuntimeError('ordinary user could review video')
                            status, response = post('/HttpService/allVideoList', {'requestId': 'list-before', 'pageIndex': 1, 'pageCount': 20})
                            if status != 200 or response.get('errorCode') != 0 or any(v['videoId'] == video_id for v in response['result']['videoList']):
                                raise RuntimeError('unreviewed video appeared publicly')
                            status, response = post('/HttpService/setLike', {'requestId': 'pending-like', 'sessionId': token, 'videoId': video_id})
                            if response.get('errorCode') != 1001: raise RuntimeError('pending video accepted interaction')
                            status, _ = post('/HttpService/statusVideoList', {'requestId': 'review-list-denied', 'sessionId': token,
                                'videoStatus': 1, 'pageIndex': 1, 'pageCount': 20})
                            if status != 403: raise RuntimeError('ordinary user accessed review queue')
                            subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-D', 'vod_test', '-e', "UPDATE vod_users SET role_type=2 WHERE email='other-e2e@example.test'"],
                                check=True, env=env, timeout=20)
                            status, response = post('/HttpService/statusVideoList', {'requestId': 'review-list', 'sessionId': guest_token,
                                'videoStatus': 1, 'pageIndex': 1, 'pageCount': 20})
                            if status != 200 or response.get('errorCode') != 0 or not any(v['videoId'] == video_id for v in response['result']['videoList']):
                                raise RuntimeError('pending video missing from review queue')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': playlist_id})
                            if status != 404: raise RuntimeError('pending playlist was public')
                            preview_url = f'http://127.0.0.1:{http_port}/HttpService/downloadVideo?' + urllib.parse.urlencode({'fileId': playlist_id, 'sessionId': guest_token})
                            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-threads', '1', '-i', preview_url,
                                '-f', 'null', '-'], check=True, timeout=30)
                            review['sessionId'] = guest_token
                            status, response = post('/HttpService/checkVideo', review)
                            if status != 200 or response.get('errorCode') != 0: raise RuntimeError('admin review failed: ' + str(response))
                            status, public_cover = binary_request('GET', '/HttpService/downloadPhoto', {'fileId': cover_id})
                            if status != 200 or public_cover != cover_bytes: raise RuntimeError('approved cover unavailable')
                            status, response = post('/HttpService/allVideoList', {'requestId': 'list-after', 'pageIndex': 1, 'pageCount': 20})
                            if status != 200 or response.get('errorCode') != 0 or not any(v['videoId'] == video_id for v in response['result']['videoList']):
                                raise RuntimeError('approved video missing from public list')
                            interaction = {'requestId': 'interaction', 'sessionId': token, 'videoId': video_id}
                            status, response = post('/HttpService/judgeLike', interaction)
                            if status != 200 or response.get('result', {}).get('isLike') is not False: raise RuntimeError('initial like state wrong')
                            status, response = post('/HttpService/setLike', interaction)
                            if response.get('result', {}).get('likeCount') != 1 or not response['result']['isLike']: raise RuntimeError('like failed')
                            status, response = post('/HttpService/setLike', interaction)
                            if response.get('result', {}).get('likeCount') != 0 or response['result']['isLike']: raise RuntimeError('unlike failed')
                            status, response = post('/HttpService/setLike', {'requestId': 'guest-like', 'videoId': video_id})
                            if status != 401: raise RuntimeError('guest could like')
                            status, response = post('/HttpService/setPlay', {'requestId': 'play', 'videoId': video_id})
                            if response.get('result', {}).get('playCount') != 1: raise RuntimeError('play count failed')
                            barrage = dict(interaction, barrageInfo={'barrageContent': "你好 ' video", 'barrageTime': 3})
                            status, response = post('/HttpService/newBarrage', barrage)
                            if status != 200 or response.get('errorCode') != 0: raise RuntimeError('add barrage failed')
                            status, response = post('/HttpService/getBarrage', {'requestId': 'barrage', 'videoId': video_id})
                            items = response.get('result', {}).get('barrageList', [])
                            if len(items) != 1 or items[0]['barrageContent'] != "你好 ' video" or items[0]['barrageTime'] != 3:
                                raise RuntimeError('barrage roundtrip failed')
                            status, response = post('/HttpService/keyVideoList', {'requestId': 'search', 'searchKey': 'test video', 'pageIndex': 1, 'pageCount': 20})
                            videos = response.get('result', {}).get('videoList', [])
                            if status != 200 or not any(v['videoId'] == video_id and v['likeCount'] == 0 and v['playCount'] == 1 for v in videos):
                                raise RuntimeError('search or video counts failed: ' + str(response))
                            status, response = post('/HttpService/keyVideoList', {'requestId': 'search-miss', 'searchKey': 'no-such-video-xyz', 'pageIndex': 1, 'pageCount': 20})
                            if response.get('result', {}).get('totalCount') != 0: raise RuntimeError('search returned unrelated videos')
                            for path, key, value, expected in [('/HttpService/typeVideoList', 'videoTypeId', 4, 1), ('/HttpService/typeVideoList', 'videoTypeId', 5, 0),
                                                               ('/HttpService/tagVideoList', 'videoTag', 7, 1), ('/HttpService/tagVideoList', 'videoTag', 9, 0)]:
                                status, response = post(path, {'requestId': 'filter', key: value, 'pageIndex': 1, 'pageCount': 20})
                                if status != 200 or response.get('result', {}).get('totalCount') != expected: raise RuntimeError('video filter failed: ' + str(response))
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': video_info['videoFileId']})
                            if status != 404: raise RuntimeError('original source was exposed after review')
                            playlist_url = f'http://127.0.0.1:{http_port}/HttpService/downloadVideo?' + urllib.parse.urlencode({'fileId': playlist_id})
                            subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-threads', '1', '-i', playlist_url,
                                '-f', 'null', '-'], check=True, timeout=30)
                            subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-D', 'vod_test', '-e', "UPDATE vod_users SET role_type=3 WHERE email='other-e2e@example.test'"], check=True, env=env, timeout=20)
                            action = {'requestId': 'halt', 'sessionId': guest_token, 'videoId': video_id}
                            status, response = post('/HttpService/haltVideo', action)
                            if response.get('errorCode') != 1001: raise RuntimeError('non-owner halted video')
                            action['sessionId'] = token
                            status, response = post('/HttpService/haltVideo', action)
                            if response.get('errorCode') != 0: raise RuntimeError('owner could not halt video')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': playlist_id})
                            if status != 404: raise RuntimeError('offline playlist was publicly accessible')
                            status, _ = binary_request('GET', '/HttpService/downloadPhoto', {'fileId': cover_id})
                            if status != 404: raise RuntimeError('offline cover was publicly accessible')
                            status, response = post('/HttpService/saleVideo', action)
                            if response.get('errorCode') != 0: raise RuntimeError('owner could not resubmit video')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': playlist_id})
                            if status != 404: raise RuntimeError('resubmitted playlist was public before approval')
                            subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-D', 'vod_test', '-e', "UPDATE vod_users SET role_type=2 WHERE email='other-e2e@example.test'"], check=True, env=env, timeout=20)
                            status, response = post('/HttpService/checkVideo', review)
                            if response.get('errorCode') != 0: raise RuntimeError('resubmitted video could not be reviewed')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': playlist_id})
                            if status != 200: raise RuntimeError('reapproved playlist did not become public')
                            subprocess.run([str(client), '--no-defaults', '--protocol=tcp', '-h', '127.0.0.1', '-P', str(port),
                                '-u', 'root', '-D', 'vod_test', '-e', "UPDATE vod_users SET role_type=3 WHERE email='other-e2e@example.test'"], check=True, env=env, timeout=20)
                            status, response = post('/HttpService/removeVideo', dict(action, sessionId=guest_token))
                            if response.get('errorCode') != 1001: raise RuntimeError('non-owner removed video')
                            status, response = post('/HttpService/removeVideo', action)
                            if response.get('errorCode') != 0: raise RuntimeError('owner could not remove video')
                            status, _ = binary_request('GET', '/HttpService/downloadVideo', {'fileId': playlist_id})
                            if status != 404: raise RuntimeError('removed playlist was publicly accessible')
                            status, response = post('/HttpService/userVideoList', {'requestId': 'deleted-list', 'sessionId': token, 'pageIndex': 1, 'pageCount': 20})
                            if any(v['videoId'] == video_id for v in response.get('result', {}).get('videoList', [])):
                                raise RuntimeError('removed video still in owner list')
                            if worker_process:
                                worker_process.terminate()
                                try: worker_process.wait(timeout=35)
                                except subprocess.TimeoutExpired: worker_process.kill(); worker_process.wait()
                            file_process.terminate()
                            file_process.wait(timeout=15)
                            print('authenticated FastDFS gateway roundtrip passed')
                    status, response = post('/HttpService/sessionLogin', {'requestId': 'test-3', 'sessionId': 'invalid'})
                    if status != 200 or response.get('errorCode') != 1001:
                        raise RuntimeError('invalid session accepted')
                    status, _ = post('/HttpService/sessionLogin', {'requestId': 'test-4'})
                    if status != 400:
                        raise RuntimeError('missing session accepted')
                    status, response = post('/HttpService/logout', {'requestId': 'test-logout', 'sessionId': token})
                    if status != 200 or response.get('errorCode') != 0:
                        raise RuntimeError('session logout failed')
                    status, response = post('/HttpService/sessionLogin', {'requestId': 'test-reuse', 'sessionId': token})
                    if status != 200 or response.get('errorCode') != 1001:
                        raise RuntimeError('logged out session accepted')
                    user.terminate()
                    user.wait(timeout=15)
                    status, _ = post('/HttpService/tempLogin', {'requestId': 'test-5'})
                    if status != 503:
                        raise RuntimeError('stopped user service did not return 503')
                    print('guest HTTP to RPC to MySQL integration passed')
                except Exception:
                    e2e_log.flush()
                    print((root / 'e2e.log').read_text(errors='replace'), file=sys.stderr)
                    raise
                finally:
                    for child in reversed(processes):
                        if child.poll() is None:
                            child.terminate()
                            try:
                                child.wait(timeout=15)
                            except subprocess.TimeoutExpired:
                                child.kill()
                                child.wait()
                    if mq_namespace:
                        subprocess.run([str(mq_cleanup), mq_namespace], check=True, env=env, timeout=20)
                    smtp.close()
        except Exception:
            output.flush()
            print(logfile.read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
