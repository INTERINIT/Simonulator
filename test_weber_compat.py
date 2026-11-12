"""Actual WinHTTP gateway compatibility tests, exclusively local fixtures.

Build test_weber_compat.cpp with the same static MinGW/WinHTTP toolchain as
the switch, then pass --harness. No account, public form, or remote site is
ever requested by these tests. The cookie strings below are synthetic.
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import socket
import ssl
import tempfile
import threading
import time
from urllib.parse import urlsplit


class Origin(BaseHTTPRequestHandler):
    observations = []

    def do_GET(self):
        path = urlsplit(self.path).path
        self.observations.append((self.path, dict(self.headers)))
        status, content_type = 200, "text/html; charset=utf-8"
        fields = []
        body = "<html><body>远东在线 UTF-8. 1 + 2.</body></html>".encode("utf-8")
        if path == "/redirect":
            status, body = 302, b""
            fields += [("Location", "/final?groupId=1&groupType=1")]
        elif path == "/credential-redirect":
            status, body = 302, b""
            fields += [("Location", "/final?account=synthetic&password=LOCAL_SECRET")]
        elif path == "/drop-query-redirect":
            status, body = 302, b""
            fields += [("Location", "../final?group=2"), ("Set-Cookie", "redirect_cookie=synthetic; Path=/")]
        elif path == "/loop":
            status, body = 302, b""
            fields += [("Location", "/loop")]
        elif path == "/unsafe-redirect":
            status, body = 302, b""
            fields += [("Location", "file:///C:/private")]
        elif path == "/json":
            content_type = "application/json; charset=UTF-8"
            body = '{"code":500,"msg":"Please log in first <b>登录</b>","data":null}'.encode("utf-8")
        elif path == "/json-suffix":
            content_type, body = "application/problem+json", b'{"title":"Synthetic error"}'
        elif path in ("/gbk", "/gb2312", "/meta-gbk"):
            encoding = "gb2312" if path == "/gb2312" else "gbk"
            content_type = "text/html" if path == "/meta-gbk" else f"text/html; charset=\"{encoding.upper()}\""
            body = ('<html><head><meta http-equiv="Content-Type" content="text/html; charset=gb2312">'
                    '</head><body>远东在线：测试 <a href="/chat?group=1">聊天</a></body></html>').encode(encoding)
        elif path in ("/long-utf8", "/long-gbk"):
            content_type = "text/plain; charset=" + ("gbk" if path.endswith("gbk") else "utf-8")
            body = ("测试中文" * 1024).encode("gbk" if path.endswith("gbk") else "utf-8")
        elif path == "/bad-utf8":
            body = b"bad\xc2\x20encoding"
        elif path == "/bad-gbk":
            content_type, body = "text/plain; charset=gbk", b"bad\x81"
        elif path == "/bom":
            content_type = "text/plain"
            body = b"\xef\xbb\xbf" + "Chinese 中文".encode("utf-8")
        elif path == "/binary":
            content_type, body = "application/octet-stream", bytes(range(255))
        elif path in ("/cookie-set", "/cookie-redirect"):
            fields += [("Set-Cookie", "login=synthetic; Path=/; HttpOnly"),
                       ("Set-Cookie", "pathonly=scoped; Path=/scope"),
                       ("Set-Cookie", "secureonly=private; Path=/; Secure"),
                       ("Set-Cookie", "foreign=bad; Domain=example.invalid; Path=/")]
            body = b"Set synthetic cookies"
            if path == "/cookie-redirect":
                status, body = 302, b""
                fields += [("Location", "/cookie-check")]
        elif path == "/cookie-expire":
            fields += [("Set-Cookie", "login=removed; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT")]
            body = b"Expired synthetic cookie"
        elif path in ("/cookie-check", "/scope/check"):
            content_type, body = "text/plain", self.headers.get("Cookie", "NO_COOKIE").encode("ascii")
        elif path == "/auth":
            status, body = 401, b"Authentication is not automatic"
            fields += [("WWW-Authenticate", "Negotiate"), ("WWW-Authenticate", "NTLM")]
        if path == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=UTF-8")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for offset in range(0, len(body), 5):
                chunk = body[offset:offset + 5]
                self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii") + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        for name, value in fields:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", type=Path, required=True)
    parser.add_argument("--switch", type=Path, help="Also test actual switch modem framing and redial cookies")
    parser.add_argument("--openssl", type=Path, help="Generate a local untrusted TLS fixture (never installed)")
    args = parser.parse_args()
    origin = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    worker = threading.Thread(target=origin.serve_forever, daemon=True)
    worker.start()
    process = subprocess.Popen([str(args.harness.resolve())], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    base = f"http://127.0.0.1:{origin.server_port}"

    def fetch(path, key=1, limit=8192):
        url = path if "://" in path else base + path
        request = f"GET {url} HTTP/1.0\r\nX-Simon-Max-Bytes: {limit}\r\n\r\n".encode("ascii")
        process.stdin.write(f"{key} {request.hex()}\n")
        process.stdin.flush()
        line = process.stdout.readline().strip()
        assert line, process.stderr.read()
        reply = bytes.fromhex(line)
        header, body = reply.split(b"\r\n\r\n", 1)
        lines = header.decode("ascii").split("\r\n")
        fields = dict(line.split(": ", 1) for line in lines[1:])
        assert int(fields["Content-Length"]) == len(body)
        assert "Transfer-Encoding" not in fields
        return int(lines[0].split()[1]), fields, body

    def forget(key):
        process.stdin.write(f"{key} forget\n")
        process.stdin.flush()
        assert process.stdout.readline().strip() == "OK"

    try:
        status, headers, body = fetch("/page")
        assert status == 200 and "远东在线" in body.decode("utf-8")
        assert headers["X-Simon-Final-URL"] == base + "/page"
        assert fetch("/chunked")[2] == body
        status, headers, _ = fetch("/redirect")
        assert status == 200 and headers["X-Simon-Final-URL"] == base + "/final?groupId=1&groupType=1"
        _, headers, _ = fetch("/credential-redirect")
        assert headers["X-Simon-Final-URL"] == base + "/final"
        before = len(Origin.observations)
        _, headers, _ = fetch("/drop-query-redirect?password=SYNTHETIC", key=31)
        assert headers["X-Simon-Final-URL"] == base + "/final?group=2"
        observed = Origin.observations[before:]
        assert [path for path, _ in observed] == ["/drop-query-redirect?password=SYNTHETIC", "/final?group=2"]
        assert "redirect_cookie=synthetic" in observed[-1][1].get("Cookie", "")
        assert "Referer" not in observed[-1][1] and "Authorization" not in observed[-1][1]
        before = len(Origin.observations)
        loop_response = fetch("/loop")
        assert loop_response[0] == 508, (loop_response, len(Origin.observations) - before)
        assert len(Origin.observations) - before == 5, "Redirect hop limit changed"
        assert fetch("/unsafe-redirect")[0] == 502
        _, headers, _ = fetch("/final?PASSWORD=LOCAL_SECRET")
        assert headers["X-Simon-Final-URL"] == base + "/final"
        _, headers, _ = fetch("/final?%70assword=LOCAL_SECRET")
        assert headers["X-Simon-Final-URL"] == base + "/final"
        _, headers, _ = fetch("/" + "x" * 231)
        assert "X-Simon-Final-URL" not in headers
        print("PASS UTF-8, exact lengths, bounded manual redirects, cookies and credential-query privacy")

        for path in ("/json", "/json-suffix"):
            status, headers, body = fetch(path)
            assert status == 200 and headers["Content-Type"] == "text/plain; charset=utf-8"
            assert body.startswith(b"{")
        assert fetch("/binary")[0] == 415
        for path in ("/gbk", "/gb2312", "/meta-gbk"):
            status, headers, body = fetch(path)
            assert status == 200 and headers["Content-Type"] == "text/html; charset=utf-8"
            assert "远东在线：测试" in body.decode("utf-8") and b'/chat?group=1' in body
        assert fetch("/bom")[2].decode("utf-8") == "Chinese 中文"
        assert fetch("/bad-utf8")[0] == 502 and fetch("/bad-gbk")[0] == 502
        print("PASS JSON safely typed as plain text, GBK/GB2312/header/meta conversion, BOM and bad encoding")

        for path in ("/long-utf8", "/long-gbk"):
            for limit in (512, 513, 514, 515):
                status, headers, body = fetch(path, limit=limit)
                assert status == 200 and headers["X-Simon-Truncated"] == "yes"
                assert len(body) <= limit and len(body) >= limit - 3
                assert body.decode("utf-8").startswith("测试中文")
        print("PASS transformed body bound and UTF-8-safe truncation at every multibyte boundary")

        status, _, body = fetch("/cookie-redirect", key=10)
        assert status == 200 and b"login=synthetic" in body
        assert b"secureonly" not in body and b"foreign" not in body and b"pathonly" not in body
        body = fetch("/scope/check", key=10)[2]
        assert b"login=synthetic" in body and b"pathonly=scoped" in body
        assert fetch("/cookie-check", key=11)[2] == b"NO_COOKIE"
        assert fetch(f"http://localhost:{origin.server_port}/cookie-check", key=10)[2] == b"NO_COOKIE"
        fetch("/cookie-expire", key=10)
        assert b"login=" not in fetch("/cookie-check", key=10)[2]
        fetch("/cookie-set", key=12)
        assert b"login=synthetic" in fetch("/cookie-check", key=12)[2]
        forget(12)
        assert fetch("/cookie-check", key=12)[2] == b"NO_COOKIE"
        print("PASS per-client cookie isolation, redirect replay, domain/path/Secure/expiry and disconnect clear")

        before = len(Origin.observations)
        assert fetch("/auth")[0] == 401
        assert len(Origin.observations) == before + 1, "Automatic authentication retry"
        assert not any("Authorization" in headers for _, headers in Origin.observations)
        assert all(headers.get("Accept-Language") == "en-US,en;q=0.9" for _, headers in Origin.observations)
        print("PASS local Windows credentials never used for Negotiate/NTLM challenge")
        if args.openssl:
            with tempfile.TemporaryDirectory(prefix="weber-untrusted-tls-") as tls_dir:
                certificate, private_key = Path(tls_dir, "cert.pem"), Path(tls_dir, "key.pem")
                subprocess.run([str(args.openssl.resolve()), "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                "-keyout", str(private_key), "-out", str(certificate), "-days", "1",
                                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                               check=True, capture_output=True)
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(certificate, private_key)
                tls = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
                tls.socket = context.wrap_socket(tls.socket, server_side=True)
                tls_worker = threading.Thread(target=tls.serve_forever, daemon=True)
                tls_worker.start()
                try:
                    before = len(Origin.observations)
                    status, _, body = fetch(f"https://localhost:{tls.server_port}/page", key=20)
                    assert status == 502 and b"WinHTTP" in body
                    assert len(Origin.observations) == before, "Untrusted TLS reached the HTTP application"
                finally:
                    tls.shutdown()
                    tls.server_close()
                    tls_worker.join(2)
            print("PASS untrusted TLS rejected before any HTTP request; certificate validation unchanged")
        if args.switch:
            # This is the real framed and paced switch path, in a separate
            # working directory and on an ephemeral loopback port.
            from test_transport import Handset
            from test_weber_gateway import send_request, receive_response, run_tests, Origin as ClassicOrigin
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                switch_port = probe.getsockname()[1]
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            with tempfile.TemporaryDirectory(prefix="weber-compat-switch-") as work:
                switch = subprocess.Popen([str(args.switch.resolve()), "--port", str(switch_port), "--data-dir", "."],
                                          cwd=work, startupinfo=startup)
                phones = []
                try:
                    time.sleep(0.5)
                    assert switch.poll() is None
                    a, b = Handset(switch_port), Handset(switch_port)
                    phones = [a, b]
                    for phone in phones:
                        phone.send("M D 5551003")
                        phone.wait_for("M C 2400")
                    send_request(a, base + "/cookie-set")
                    receive_response(a)
                    a.send("M H")
                    a.wait_for("M H")
                    a.send("M D 5551003")
                    a.wait_for("M C 2400")
                    send_request(a, base + "/cookie-check")
                    assert b"login=synthetic" in receive_response(a)[1]
                    send_request(b, base + "/cookie-check")
                    assert receive_response(b)[1] == b"NO_COOKIE"
                    send_request(a, base + "/final?password=LOCAL_SECRET")
                    header, _ = receive_response(a)
                    assert b"LOCAL_SECRET" not in header
                    send_request(a, base + "/json")
                    header, body = receive_response(a)
                    assert b"Content-Type: text/plain; charset=utf-8" in header
                    assert b"Please log in first" in body
                    trace = Path(work, "amps-switch.log").read_text(encoding="utf-8")
                    assert "LOCAL_SECRET" not in trace and "password=" not in trace
                    print("PASS switch: distinct handset jars, hang-up/redial persistence, JSON and private logs")
                    classic = ThreadingHTTPServer(("127.0.0.1", 0), ClassicOrigin)
                    classic_worker = threading.Thread(target=classic.serve_forever, daemon=True)
                    classic_worker.start()
                    try:
                        run_tests(switch_port, classic.server_port)
                    finally:
                        classic.shutdown()
                        classic.server_close()
                        classic_worker.join(2)
                finally:
                    for phone in phones:
                        phone.close()
                    switch.terminate()
                    switch.wait(timeout=5)
    finally:
        process.stdin.close()
        process.wait(timeout=20)
        origin.shutdown()
        origin.server_close()
        worker.join(2)
    assert process.returncode == 0, process.stderr.read()


if __name__ == "__main__":
    main()
