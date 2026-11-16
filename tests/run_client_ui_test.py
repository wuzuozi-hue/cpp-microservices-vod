import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

requests = []
class Handler(BaseHTTPRequestHandler):
    uploaded_photo = b''
    def log_message(self, *_): pass
    def do_GET(self):
        path = urllib.parse.urlsplit(self.path)
        if path.path != '/HttpService/downloadPhoto': self.send_error(404); return
        assert urllib.parse.parse_qs(path.query)['fileId'] == ['test-cover']
        self.send_response(200); self.send_header('Content-Type', 'image/png')
        self.send_header('Content-Length', str(len(self.uploaded_photo))); self.end_headers(); self.wfile.write(self.uploaded_photo)
    def do_POST(self):
        path = urllib.parse.urlsplit(self.path)
        body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        if path.path.endswith('/uploadVideo'):
            query = urllib.parse.parse_qs(path.query)
            assert query['sessionId'] == ['test-session'] and body == b'test video bytes'
            response = {'requestId': query['requestId'][0], 'errorCode': 0, 'errorMsg': '', 'result': {'fileId': 'test-file'}}
        elif path.path.endswith('/uploadPhoto'):
            query = urllib.parse.parse_qs(path.query)
            assert query['sessionId'] == ['test-session'] and body.startswith(b'\x89PNG\r\n\x1a\n')
            type(self).uploaded_photo = body
            response = {'requestId': query['requestId'][0], 'errorCode': 0, 'errorMsg': '', 'result': {'fileId': 'test-cover'}}
        else:
            data = json.loads(body)
            response = {'requestId': data['requestId'], 'errorCode': 0, 'errorMsg': ''}
            if path.path.endswith('/tempLogin'): response['result'] = {'sessionId': 'test-session'}
            elif path.path.endswith('/getCode'):
                assert data['email'] == 'student@example.test' and data['sessionId'] == 'test-session'
                response['result'] = {'codeId': 'test-code-id'}
            elif path.path.endswith('/vcodeLogin'):
                assert data['codeId'] == 'test-code-id' and data['verifyCode'] == '123456'
            elif path.path.endswith('/getUserInfo'):
                response['result'] = {'userInfo': {'email': 'student@example.test', 'nickname': ''}}
            elif path.path.endswith('/setNickname'):
                assert data['nickname'] == 'student'
            elif path.path.endswith('/setPassword'):
                assert data['password'] == 'practicePassword9!'
            elif path.path.endswith('/setAvatar'):
                assert data['fileId'] == 'test-cover' and data['sessionId'] == 'test-session'
            elif path.path.endswith('/passwdLogin'):
                assert data['username'] == 'student' and data['password'] == 'practicePassword9!'
            elif path.path.endswith('/newVideo'):
                assert data['videoInfo']['videoFileId'] == 'test-file' and data['videoInfo']['videoTitle'] == 'Test title'
                assert data['videoInfo']['videoType'] == 4 and data['videoInfo']['duration'] == 12 and data['videoInfo']['videoTag'] == [7, 8]
                assert data['videoInfo']['photoFileId'] == 'test-cover'
                response['result'] = {'videoId': 'test-video-id'}
            elif path.path.endswith('/userVideoList'):
                assert data['sessionId'] == 'test-session'
                response['result'] = {'videoList': [{'videoTitle': 'My video', 'videoId': 'test-video-id', 'videoStatus': 1}]}
            elif path.path.endswith('/haltVideo') or path.path.endswith('/saleVideo') or path.path.endswith('/removeVideo'):
                assert data['sessionId'] == 'test-session' and data['videoId'] == 'test-video-id'
            elif path.path.endswith('/keyVideoList'):
                assert data['pageIndex'] == 1 and data['searchKey'] in ('Public', 'Slow', 'Fast')
                if data['searchKey'] == 'Slow': time.sleep(0.7)
                title = {'Slow': 'Slow video', 'Fast': 'Fast video', 'Public': 'Public video'}[data['searchKey']]
                response['result'] = {'videoList': [{'videoId': 'public-id', 'userId': 'author-id', 'videoTitle': title, 'videoDesc': 'test', 'videoFileId': 'playlist'}]}
            elif path.path.endswith('/newAttention') or path.path.endswith('/delAttention'):
                assert data['userId'] == 'author-id' and data['sessionId'] == 'test-session'
            elif path.path.endswith('/setLike'):
                assert data['videoId'] == 'public-id' and data['sessionId'] == 'test-session'
                response['result'] = {'isLike': True, 'likeCount': 1}
            elif path.path.endswith('/newBarrage'):
                assert data['videoId'] == 'public-id' and data['barrageInfo'] == {'barrageContent': 'hello', 'barrageTime': 3}
            elif path.path.endswith('/getBarrage'):
                assert data['videoId'] == 'public-id'
                response['result'] = {'barrageList': [{'barrageContent': 'hello', 'barrageTime': 3}]}
            elif path.path.endswith('/statusVideoList'):
                response['result'] = {'videoList': [{'videoTitle': 'Pending', 'videoId': 'test-video-id', 'videoFileId': 'playlist'}]}
            elif path.path.endswith('/checkVideo'):
                assert data['videoId'] == 'test-video-id' and data['checkResult'] is True
            elif path.path.endswith('/newAdministrator'):
                assert data['userInfo']['email'] == 'admin@example.test' and data['userInfo']['roleType'] == 2
            elif path.path.endswith('/getAdminListByStatus'):
                response['result'] = {'userList': [{'userId': 'admin-id', 'email': 'admin@example.test', 'nickname': 'reviewer', 'userStatus': 1, 'userMemo': 'note'}]}
            elif path.path.endswith('/delAdministrator'):
                assert data['userId'] == 'admin-id'
            elif not path.path.endswith('/logout'):
                self.send_error(404); return
        requests.append(path.path)
        output = json.dumps(response).encode(); self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(output))); self.end_headers(); self.wfile.write(output)

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
try:
    with tempfile.TemporaryDirectory(prefix='vod-ui-test-') as folder:
        video = Path(folder) / 'sample.mp4'; video.write_bytes(b'test video bytes')
        env = os.environ.copy(); env['QT_QPA_PLATFORM'] = 'offscreen'
        subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}', str(video)], env=env, check=True, timeout=45)
    expected = ['tempLogin', 'getCode', 'vcodeLogin', 'getUserInfo', 'setNickname', 'setPassword', 'uploadPhoto', 'setAvatar', 'uploadVideo', 'uploadPhoto', 'newVideo', 'userVideoList', 'haltVideo', 'saleVideo', 'removeVideo',
                'keyVideoList', 'keyVideoList', 'keyVideoList', 'newAttention', 'delAttention', 'setLike', 'newBarrage', 'getBarrage', 'statusVideoList', 'checkVideo', 'newAdministrator', 'getAdminListByStatus', 'delAdministrator', 'logout', 'tempLogin', 'passwdLogin']
    assert [path.rsplit('/', 1)[-1] for path in requests] == expected
finally:
    server.shutdown(); server.server_close(); thread.join()
