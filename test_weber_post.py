"""Local-only POST origin + actual WinHTTP/switch regression. No real chat.

All posted data is synthetic; the two HTTP origins bind loopback and record
requests only in memory. Optional switch uses a temporary directory/port.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from urllib.parse import urlsplit


class Origin(BaseHTTPRequestHandler):
    observations = []
    foreign_port = 0

    def reply(self, method, body=b''):
        self.observations.append((method, self.path, body, dict(self.headers), self.server.server_port))
        path = urlsplit(self.path).path
        if path == '/drop':
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        status, destination = 200, None
        if path.startswith('/r') and path[2:] in ('301', '302', '303', '307', '308'):
            status = int(path[2:])
            destination = '/after?room=7'
        elif path == '/cross307':
            status, destination = 307, f'http://localhost:{self.server.server_port}/after'
        elif path == '/port308':
            status, destination = 308, f'http://127.0.0.1:{self.foreign_port}/after'
        elif path == '/cross303':
            status, destination = 303, f'http://localhost:{self.server.server_port}/after'
        elif path == '/postloop':
            status, destination = 307, '/postloop'
        result = b'LOCAL POST FIXTURE OK'
        self.send_response(status)
        self.send_header('Content-Type', 'text/plain; charset=us-ascii')
        self.send_header('Content-Length', str(len(result)))
        if destination:
            self.send_header('Location', destination)
            self.send_header('Set-Cookie', 'posted=synthetic; Path=/')
        self.end_headers()
        self.wfile.write(result)

    def do_POST(self):
        self.reply('POST', self.rfile.read(int(self.headers.get('Content-Length', '0'))))

    def do_GET(self):
        self.reply('GET')

    def log_message(self, *_):
        pass


def request(url, body=b'message=LOCAL_ONLY%2B%26+%E6%B5%8B%E8%AF%95', *, headers=None):
    fields = (f'Content-Type: application/x-www-form-urlencoded\r\nContent-Length: {len(body)}\r\n'
              if headers is None else headers)
    return (f'POST {url} HTTP/1.0\r\n{fields}\r\n').encode('ascii') + body


def unpack(reply):
    header, body = reply.split(b'\r\n\r\n', 1)
    lines = header.split(b'\r\n')
    fields = dict(line.split(b': ', 1) for line in lines[1:])
    assert int(fields[b'Content-Length']) == len(body)
    return int(lines[0].split()[1]), fields, body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--switch', type=Path)
    args = parser.parse_args()
    origins = [ThreadingHTTPServer(('127.0.0.1', 0), Origin) for _ in range(2)]
    workers = [threading.Thread(target=server.serve_forever, daemon=True) for server in origins]
    for worker in workers:
        worker.start()
    Origin.foreign_port = origins[1].server_port
    base = f'http://127.0.0.1:{origins[0].server_port}'
    process = subprocess.Popen([str(args.harness.resolve())], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def fetch(wire):
        process.stdin.write('801 ' + wire.hex() + '\n')
        process.stdin.flush()
        return unpack(bytes.fromhex(process.stdout.readline().strip()))

    try:
        sent = request(base + '/echo?room=1')
        expected = sent.split(b'\r\n\r\n', 1)[1]
        assert fetch(sent)[0] == 200
        assert Origin.observations[-1][:3] == ('POST', '/echo?room=1', expected)
        assert Origin.observations[-1][3].get('Content-Type') == 'application/x-www-form-urlencoded'
        assert fetch(request(base + '/echo', b'x' * 1018))[0] == 200
        assert len(Origin.observations[-1][2]) == 1018
        print('PASS actual WinHTTP POST body unchanged, UTF-8 percent encoding, action query and 1018-byte maximum')

        for code in (301, 302, 303, 307, 308):
            before = len(Origin.observations)
            status, headers, _ = fetch(request(base + f'/r{code}?old=not_carried'))
            observed = Origin.observations[before:]
            assert status == 200 and len(observed) == 2
            assert observed[0][0] == 'POST' and observed[0][2] == expected
            assert observed[1][1] == '/after?room=7'
            assert observed[1][0] == ('POST' if code in (307, 308) else 'GET')
            assert observed[1][2] == (expected if code in (307, 308) else b'')
            assert 'posted=synthetic' in observed[1][3].get('Cookie', '')
            if code not in (307, 308):
                assert 'Content-Type' not in observed[1][3]
            assert headers[b'X-Simon-Final-URL'].decode() == base + '/after?room=7'
        for path in ('/cross307', '/port308'):
            before = len(Origin.observations)
            assert fetch(request(base + path))[0] == 502
            assert len(Origin.observations) == before + 1, 'POST leaked to a second origin'
        before = len(Origin.observations)
        assert fetch(request(base + '/cross303'))[0] == 200
        assert Origin.observations[before+1][:3] == ('GET', '/after', b'')
        assert 'posted=' not in Origin.observations[before+1][3].get('Cookie', '')
        before = len(Origin.observations)
        assert fetch(request(base + '/postloop'))[0] == 508
        assert len(Origin.observations) == before + 5
        print('PASS 301/302/303 Post-Redirect-Get, same-origin 307/308, cookies, origin/body isolation and hop limit')

        before = len(Origin.observations)
        for wire, status in (
            (request(base+'/echo', b'', headers='Content-Type: application/x-www-form-urlencoded\r\n'), 411),
            (request(base+'/echo', b'', headers='Content-Length: 0\r\n'), 415),
            (request(base+'/echo', b'abc', headers='Content-Length: 3\r\nContent-Length: 4\r\n'), 400),
            (request(base+'/echo', b'abc', headers='Transfer-Encoding: chunked\r\n'), 400),
            (request(base+'/echo', b'x'*1019), 413),
            (sent[:-1], 400), (sent+b'EXTRA', 400),
        ):
            assert fetch(wire)[0] == status
        assert len(Origin.observations) == before
        before = len(Origin.observations)
        assert fetch(request(base+'/drop'))[0] == 502
        assert len(Origin.observations) == before + 1, 'A failed POST was automatically retried'
        print('PASS missing/conflicting/oversized/extra/incomplete bodies and chunking rejected; dropped POST never retried')

        if args.switch:
            from test_transport import Handset
            from test_weber_gateway import receive_response
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            with tempfile.TemporaryDirectory(prefix='weber-post-switch-') as work:
                switch = subprocess.Popen([str(args.switch.resolve()), '--port', str(port), '--data-dir', '.'], cwd=work,
                                          startupinfo=startup, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                phone = None
                try:
                    time.sleep(0.5)
                    phone = Handset(port)
                    phone.send('M D 5551003')
                    phone.wait_for('M C 2400')
                    wire = request(base+'/echo?room=2', b'message=LOCAL_SWITCH_ONLY')
                    before = len(Origin.observations)
                    for byte in wire[:-1]:
                        phone.send('M X ' + bytes([byte]).hex())
                    time.sleep(0.15)
                    assert len(Origin.observations) == before, 'Partial POST submitted before body completion'
                    assert not any(line.startswith('M X ') for line in phone.drain(0.05))
                    phone.send('M X ' + wire[-1:].hex())
                    header, _ = receive_response(phone)
                    assert header.startswith(b'HTTP/1.0 200 ')
                    assert len(Origin.observations) == before + 1
                    assert Origin.observations[-1][2] == b'message=LOCAL_SWITCH_ONLY'
                    phone.send('M H')
                    phone.wait_for('M H')
                    phone.send('M D 5551003')
                    phone.wait_for('M C 2400')
                    invalid = (
                        (request(base+'/echo', b'', headers='Content-Length: 0\r\nContent-Length: 1\r\n'), 400),
                        (request(base+'/echo', b'', headers='Content-Type: application/x-www-form-urlencoded\r\n'), 411),
                        (request(base+'/echo') + b'EXTRA', 400),
                        (request(base+'/echo', b'', headers='Content-Length: 1019\r\n'), 413),
                        (request(base+'/echo', b'', headers='Transfer-Encoding: chunked\r\n'), 400),
                    )
                    for malformed, status in invalid:
                        before = len(Origin.observations)
                        phone.send('M X ' + malformed.hex())
                        header, _ = receive_response(phone)
                        assert header.startswith(f'HTTP/1.0 {status} '.encode('ascii'))
                        assert len(Origin.observations) == before
                        phone.send('M H')
                        phone.wait_for('M H')
                        phone.send('M D 5551003')
                        phone.wait_for('M C 2400')
                    trace = Path(work, 'amps-switch.log').read_text(encoding='utf-8')
                    assert 'LOCAL_SWITCH_ONLY' not in trace and 'message=' not in trace and '/echo' not in trace
                    print('PASS actual switch waits final POST byte, posts once, rejects ambiguous framing and keeps private logs')
                finally:
                    if phone:
                        phone.close()
                    switch.terminate()
                    switch.wait(timeout=10)
    finally:
        process.stdin.close()
        process.wait(timeout=20)
        for origin in origins:
            origin.shutdown()
            origin.server_close()
        for worker in workers:
            worker.join(2)
    assert process.returncode == 0


if __name__ == '__main__':
    main()
