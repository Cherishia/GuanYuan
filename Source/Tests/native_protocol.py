"""Safe native-server protocol checks. Every launched server uses a mock power executor."""
import argparse
import base64
import hashlib
import hmac
import json
import pathlib
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.request


def oaep_encrypt(payload, modulus, exponent):
    """Independent RSA-OAEP/SHA256 reference encoder for interoperability tests only."""
    n = int.from_bytes(base64.b64decode(modulus), 'big')
    e = int.from_bytes(base64.b64decode(exponent), 'big')
    size = (n.bit_length() + 7) // 8
    def mgf(seed, length):
        return b''.join(hashlib.sha256(seed + i.to_bytes(4, 'big')).digest()
                        for i in range((length + 31) // 32))[:length]
    def xor(a, b):
        return bytes(x ^ y for x, y in zip(a, b))
    db = hashlib.sha256(b'').digest() + bytes(size - len(payload) - 66) + b'\x01' + payload
    seed = secrets.token_bytes(32)
    masked_db = xor(db, mgf(seed, size - 33))
    encoded = b'\0' + xor(seed, mgf(masked_db, 32)) + masked_db
    return base64.b64encode(pow(int.from_bytes(encoded, 'big'), e, n).to_bytes(size, 'big')).decode()


class Server:
    def __init__(self, exe, folder, kind='--integration-server', discovery=False, extra=None):
        self.file = pathlib.Path(folder).resolve() / ('fixture-' + secrets.token_hex(5) + '.json')
        args = [str(pathlib.Path(exe).resolve()), kind, str(self.file)]
        if discovery:
            args.append('--integration-discovery')
        args.extend(extra or [])
        self.process = subprocess.Popen(args, creationflags=subprocess.CREATE_NO_WINDOW)
        until = time.monotonic() + 10
        while not self.file.exists():
            if self.process.poll() is not None or time.monotonic() > until:
                raise RuntimeError('Mock server did not become ready')
            time.sleep(.05)
        self.fixture = json.loads(self.file.read_text(encoding='utf-8-sig'))
        self.port = self.fixture['port']
        self.kind = kind
    def http(self, path, body=None, content_type='application/json'):
        request = urllib.request.Request('http://127.0.0.1:%d%s' % (self.port, path))
        if body is not None:
            request.data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
            request.add_header('Content-Type', content_type)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            response = opener.open(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.code, json.load(response)
    def raw(self, request):
        with socket.create_connection(('127.0.0.1', self.port), timeout=4) as connection:
            connection.sendall(request)
            return connection.recv(8192).decode('utf-8', errors='replace')
    def close(self):
        if self.kind == '--integration-server':
            pathlib.Path(str(self.file) + '.stop').touch()
        try:
            self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)


class Phone:
    def __init__(self, server):
        self.server = server
        self.id = secrets.token_hex(16)
        self.key = secrets.token_bytes(32)
    def pair_body(self, code, wrong_fingerprint=False):
        info = self.server.fixture['info']
        fingerprint = base64.b64encode(hashlib.sha256(('GuanYuan/pair/v2\n'+info['id']+'\n'+info['modulus']+'\n'+info['exponent']).encode()).digest()).decode()
        if wrong_fingerprint: fingerprint = base64.b64encode(bytes(32)).decode()
        data = json.dumps({'f': fingerprint, 'i': self.id, 'n': 'Android 娴嬭瘯', 's': base64.b64encode(self.key).decode(), 'p': code}, ensure_ascii=False, separators=(',', ':')).encode()
        return {'version': 2, 'data': oaep_encrypt(data, info['modulus'], info['exponent'])}
    def pair(self):
        return self.server.http('/pair', self.pair_body(self.server.fixture['code']))
    def body(self, action, delay=0, timestamp=None):
        stamp = int(time.time()) if timestamp is None else timestamp
        nonce = secrets.token_hex(16)
        canonical = '\n'.join([self.id, str(stamp), nonce, action, str(delay)]).encode()
        signature = base64.b64encode(hmac.new(self.key, canonical, hashlib.sha256).digest()).decode()
        return {'clientId': self.id, 'timestamp': stamp, 'nonce': nonce, 'action': action, 'delay': delay, 'signature': signature}
    def command(self, action, delay=0):
        return self.server.http('/command', self.body(action, delay))


def main():
    args = argparse.ArgumentParser()
    args.add_argument('exe'); args.add_argument('--report', required=True)
    options = args.parse_args()
    report = pathlib.Path(options.report).resolve()
    lines = []
    def check(condition, label):
        if not condition:
            raise AssertionError(label)
        lines.append('PASS: ' + label)
        print(lines[-1], flush=True)
    server = Server(options.exe, report.parent, discovery=True)
    try:
        check(server.fixture['info']['simulated'], 'Server is explicitly in simulation mode')
        check(server.http('/info')[0] == 200, 'Native information endpoint')
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(3)
            udp.sendto(b'PALM_POWER_DISCOVER_V1!', ('127.0.0.1', 18678))
            found = json.loads(udp.recv(4096))
            check(found['id'] == server.fixture['info']['id'] and found['port'] == server.port, 'UDP automatic discovery round trip')
        check(server.http('/command', {})[0] == 403, 'Unpaired command rejected')
        check(server.http('/missing')[0] == 404, 'Unknown route rejected')
        phone = Phone(server)
        check(server.http('/pair', {'version': 1, 'data': ''})[0] == 426, 'Legacy pairing protocol refused')
        check(server.http('/pair', phone.pair_body(server.fixture['code'], True))[0] == 403, 'Substituted computer fingerprint refused even with correct PIN')
        check(server.http('/pair', phone.pair_body('wrong'))[0] == 403, 'Invalid pairing PIN rejected')
        check(phone.pair()[0] == 200, 'Independent Python OAEP pairing accepted')
        check(phone.command('status')[0] == 200, 'HMAC status accepted')
        forged = phone.body('shutdown', 30); forged['signature'] = base64.b64encode(bytes(32)).decode()
        check(server.http('/command', forged)[0] == 403, 'Forged command rejected')
        check(server.http('/command', phone.body('status', timestamp=int(time.time()) - 121))[0] == 403, 'Expired timestamp rejected')
        check(server.http('/command', phone.body('status', timestamp=int(time.time()) + 121))[0] == 403, 'Future timestamp rejected')
        replay = phone.body('status')
        check(server.http('/command', replay)[0] == 200 and server.http('/command', replay)[0] == 409, 'Replay rejected')
        for action in ['shutdown', 'sleep', 'hibernate']:
            code, result = phone.command(action, 30)
            check(code == 200 and result['action'] == action and 28 <= result['remaining'] <= 30, action + ' starts countdown')
            check(phone.command('shutdown', 30)[0] == 409, action + ' blocks overlapping commands')
            code, result = phone.command('cancel')
            check(code == 200 and result['action'] == '', action + ' can be cancelled')
        check(phone.command('format', 30)[0] == 400, 'Arbitrary system commands are not supported')
        check(phone.command('shutdown', 1)[0] == 400 and phone.command('shutdown', 3601)[0] == 400, 'Countdown limits enforced over network')
        check(server.http('/command', {}, 'text/plain')[0] == 415, 'Non-JSON content type rejected')
        check('413' in server.raw(b'POST /pair HTTP/1.1\r\nHost: local\r\nContent-Type: application/json\r\nContent-Length: 5000\r\n\r\n'), 'Oversized payload rejected')
        check('400' in server.raw(b'POST /pair HTTP/1.1\r\nHost: local\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}'), 'Duplicate HTTP framing rejected')
        check('400' in server.raw(b'POST /pair HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n'), 'Chunked framing rejected')
        check(server.http('/command', b'\xff')[0] == 400, 'Invalid UTF-8 request rejected')
        check(phone.command('sleep', 10)[0] == 200, 'Real timer armed with mock executor')
        time.sleep(10.5)
        _, result = phone.command('status')
        check(result['action'] == '' and result['mockExecutions'] == 1, 'Timer executes mock action once at deadline')
        check(all(phone.command('status')[0] == 200 for _ in range(100)), '100 authenticated requests remain functional')
        check(server.http('/pair', phone.pair_body(server.fixture['code']))[0] == 403, 'Successful pairing closes pairing window')
        rate_server = Server(options.exe, report.parent)
        try:
            other = Phone(rate_server)
            for _ in range(5):rate_server.http('/pair',other.pair_body('wrong'))
            check(rate_server.http('/pair',other.pair_body('wrong'))[0] == 429,'PIN brute-force rate limiting')
        finally:rate_server.close()
        lines.append('No real power operation, firewall rule, or startup registration was changed.')
    finally:
        server.close()
        report.write_text('\n'.join(lines), encoding='utf-8')


if __name__ == '__main__':
    main()
