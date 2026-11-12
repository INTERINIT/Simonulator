"""Black-box dial-up web gateway regression, using only local HTTP fixtures.

This checks actual M X byte transport through the switch. Guest CPU/UART and
NAV rendering are deliberately separate end-to-end tests, not claimed here.
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

from test_transport import Handset


class Origin(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/slow":
            time.sleep(1.5)
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/page")
            self.end_headers()
            return
        body = b"<html><body>Origin HTML: a+b ++ +++ ++++ &amp; done.</body></html>"
        if self.path == "/large":
            body = b"abcdef0123456789" * 100
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=us-ascii")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def send_request(unit, url, maximum=8192, method="GET"):
    request = f"{method} {url} HTTP/1.0\r\nX-Simon-Max-Bytes: {maximum}\r\n\r\n".encode("ascii")
    # Deliberately fragment both request line and HTTP header boundary across
    # separate transport frames, as a real 8250 transmitter does.
    for offset in range(0, len(request), 7):
        unit.send("M X " + request[offset:offset + 7].hex())


def receive_response(unit, timeout=10):
    result = bytearray()
    start = time.monotonic()
    length = None
    body_start = None
    while time.monotonic() - start < timeout:
        for line in unit.drain(0.02):
            if not line.startswith("M X "):
                continue
            packet = bytes.fromhex(line[4:])
            assert len(packet) <= 32, "Modem reply was not bounded/paced"
            result.extend(packet)
        if body_start is None and b"\r\n\r\n" in result:
            header, _ = bytes(result).split(b"\r\n\r\n", 1)
            body_start = len(header) + 4
            fields = dict(line.split(b": ", 1) for line in header.split(b"\r\n")[1:])
            length = int(fields[b"Content-Length"])
            assert fields[b"Connection"] == b"keep-alive"
            assert b"Transfer-Encoding" not in fields
        if length is not None and len(result) >= body_start + length:
            assert len(result) == body_start + length, "Content-Length mismatch"
            elapsed = time.monotonic() - start
            assert elapsed >= max(0, (len(result) - 32) / 240 - 0.1), "Page bypassed modem pacing"
            return bytes(result[:body_start]), bytes(result[body_start:])
    raise AssertionError(f"Incomplete HTTP response after {timeout}s: {result[:160]!r}")


def run_tests(port, origin_port):
    units = []
    try:
        web, caller, called = (Handset(port) for _ in range(3))
        units.extend((web, caller, called))
        web.send("M D 5551003")
        web.wait_for("M C 2400")
        send_request(web, "http://simon.local/")
        header, body = receive_response(web)
        assert header.startswith(b"HTTP/1.0 200 ")
        assert b"http://simon.local/about" in body
        send_request(web, "http://simon.local/about")
        _, body = receive_response(web)
        assert b"About SimonWeber" in body
        print("PASS built-in linked HTML pages and persistent serial bearer")

        send_request(web, f"http://127.0.0.1:{origin_port}/redirect")
        _, body = receive_response(web)
        assert b"Origin HTML: a+b ++ +++ ++++" in body
        print("PASS real HTTP origin, redirect, raw HTML and plus-byte preservation at switch")

        send_request(web, f"http://127.0.0.1:{origin_port}/large", 512)
        header, body = receive_response(web)
        assert len(body) == 512 and b"X-Simon-Truncated: yes" in header
        print("PASS guest-requested page bound and exact truncated Content-Length")

        send_request(web, "file:///C:/Windows/win.ini")
        header, _ = receive_response(web)
        assert header.startswith(b"HTTP/1.0 400 ")
        send_request(web, "http://simon.local/", method="DELETE")
        header, _ = receive_response(web)
        assert header.startswith(b"HTTP/1.0 405 ")
        print("PASS unsupported schemes/methods return bounded HTTP errors")

        send_request(web, f"http://127.0.0.1:{origin_port}/slow")
        caller.send(f"O {called.number}")
        delay, _ = called.wait_for(f"R {caller.number}")
        assert delay < 0.8, f"HTTP worker blocked incoming voice call for {delay:.2f}s"
        called.send("A")
        delay, _ = caller.wait_for("C")
        assert delay < 0.25
        caller.send("H")
        called.wait_for("H")
        _, body = receive_response(web)
        assert b"Origin HTML" in body
        print("PASS pending web request does not block voice paging/answer/hang-up")

        send_request(web, f"http://127.0.0.1:{origin_port}/slow")
        time.sleep(0.1)
        web.send("M H")
        web.wait_for("M H")
        web.send("M D 5551003")
        web.wait_for("M C 2400")
        send_request(web, "http://simon.local/test")
        _, body = receive_response(web)
        assert b"Link works" in body and b"Origin HTML" not in body
        assert b"1 + 2 = 3" in body
        print("PASS stale HTTP completion discarded across hang-up/redial")

        web.send("M H")
        web.wait_for("M H")
        web.send("O 5551003")
        _, control = web.wait_for("C")
        assert any(line.startswith("D 1 ") and " 334 " not in line for line in control)
        assert not any(line.startswith("M C") for line in control), "RF origination prematurely put COM2 online"
        send_request(web, "http://simon.local/test")
        _, body = receive_response(web)
        assert b"Link works" in body
        web.send("M H")
        _, released = web.wait_for("M H")
        released += web.drain(0.05)
        assert "H" in released and "D 1 334 6000" in released
        print("PASS RF-first web origination assigns channel/Conversation without premature modem CONNECT")
    finally:
        for unit in units:
            unit.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5673)
    parser.add_argument("--executable", type=Path, default=Path(__file__).with_name("Simon_AMPS_Switch.exe"))
    args = parser.parse_args()
    with socket.socket() as check:
        check.bind(("127.0.0.1", args.port))
    origin = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    origin_thread = threading.Thread(target=origin.serve_forever, daemon=True)
    origin_thread.start()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    with tempfile.TemporaryDirectory(prefix="simon-weber-test-") as work:
        process = subprocess.Popen([str(args.executable.resolve()), "--port", str(args.port), "--data-dir", "."],
            cwd=work, startupinfo=startup)
        try:
            time.sleep(0.4)
            assert process.poll() is None
            run_tests(args.port, origin.server_port)
        finally:
            process.terminate()
            process.wait(timeout=5)
            origin.shutdown()
            origin.server_close()
            origin_thread.join(2)


if __name__ == "__main__":
    main()
