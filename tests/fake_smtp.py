import base64
import queue
import socketserver
import ssl
import threading


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        connection = self.request
        connection.settimeout(10)
        connection.sendall(b'220 localhost test SMTP\r\n')
        encrypted = False
        authenticated = False
        def line():
            data = bytearray()
            while not data.endswith(b'\r\n'):
                part = connection.recv(1)
                if not part:
                    return b''
                data.extend(part)
                if len(data) > 100000:
                    raise ValueError('SMTP line too large')
            return bytes(data)
        try:
            while True:
                command = line()
                if not command:
                    break
                verb = command.split(b' ', 1)[0].strip().upper()
                if verb in (b'EHLO', b'HELO'):
                    connection.sendall(b'250-localhost\r\n250 STARTTLS\r\n' if not encrypted else b'250-localhost\r\n250 AUTH LOGIN\r\n')
                elif verb == b'STARTTLS' and not encrypted:
                    connection.sendall(b'220 Ready for TLS\r\n')
                    connection = self.server.context.wrap_socket(connection, server_side=True)
                    encrypted = True
                elif verb == b'AUTH' and encrypted and command.strip().upper().startswith(b'AUTH LOGIN'):
                    parts = command.strip().split()
                    if len(parts) > 2: username = base64.b64decode(parts[2])
                    else:
                        connection.sendall(b'334 VXNlcm5hbWU6\r\n'); username = base64.b64decode(line().strip())
                    connection.sendall(b'334 UGFzc3dvcmQ6\r\n'); password = base64.b64decode(line().strip())
                    authenticated = username == b'test-user' and password == b'test-password'
                    connection.sendall(b'235 authenticated\r\n' if authenticated else b'535 denied\r\n')
                elif verb in (b'MAIL', b'RCPT') and encrypted and authenticated:
                    connection.sendall(b'250 OK\r\n')
                elif verb == b'DATA' and encrypted:
                    connection.sendall(b'354 End data with dot\r\n')
                    content = bytearray()
                    while True:
                        part = line()
                        if not part or part == b'.\r\n':
                            break
                        content.extend(part)
                        if len(content) > 100000:
                            raise ValueError('SMTP message too large')
                    self.server.messages.put(bytes(content))
                    connection.sendall(b'250 queued\r\n')
                elif verb == b'QUIT':
                    connection.sendall(b'221 Bye\r\n')
                    break
                else:
                    connection.sendall(b'500 unsupported\r\n')
        finally:
            connection.close()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def __init__(self, certificate, key):
        self.messages = queue.Queue()
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(certificate, key)
        super().__init__(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
    def close(self):
        self.shutdown()
        self.server_close()
        self.thread.join()
