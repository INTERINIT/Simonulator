"""Exercise first start, GUI settings and a mail delivery across a process restart."""
import argparse
import ctypes
from ctypes import wintypes
from contextlib import closing
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time

from test_transport import Handset

OPERATOR, APPLY = 112, 113
MAIL_NAME, MAIL_PASSWORD, REGISTER = 115, 116, 117
BIND_PORT, SAVE_LISTENER = 119, 120
LANGUAGE_CHINESE = 40002


ui = ctypes.windll.user32
ui.FindWindowExW.argtypes = (wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR)
ui.FindWindowExW.restype = wintypes.HWND
ui.GetDlgItem.argtypes = (wintypes.HWND, ctypes.c_int)
ui.GetDlgItem.restype = wintypes.HWND
ui.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
ui.SendMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
ui.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)


def wait_for(check):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.04)
    raise AssertionError(f"Timed out waiting for {check}")


def window_for(process, window_class):
    handle = None
    while handle := ui.FindWindowExW(None, handle, window_class, None):
        pid = wintypes.DWORD()
        ui.GetWindowThreadProcessId(handle, ctypes.byref(pid))
        if pid.value == process.pid:
            return handle


def edit(window, control, text):
    buffer = ctypes.create_unicode_buffer(text)
    handle = ui.GetDlgItem(window, control)
    assert handle and ui.SendMessageW(handle, 0x000c, 0, ctypes.addressof(buffer))


def click(window, control):
    assert ui.PostMessageW(window, 0x111, control, ui.GetDlgItem(window, control) or 0)


def scalar(database, sql, parameters=()):
    with closing(sqlite3.connect(database)) as db:
        return db.execute(sql, parameters).fetchone()[0]


def available_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def commands(unit, lines):
    body = "".join(line + "\r\n" for line in lines).encode("ascii")
    unit.send("M X " + body.hex())
    return b"".join(bytes.fromhex(line[4:]) for line in unit.drain(.45) if line.startswith("M X "))


def registration(unit, name):
    body = "user=" + name + "&password=test-pass"
    request = ("POST http://simon.local/register HTTP/1.0\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\n"
               f"Content-Length: {len(body)}\r\n\r\n{body}")
    unit.send("M X " + request.encode("ascii").hex())


def pending_accounts(exe, folder):
    data = folder / "account load"
    port = available_port()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    process = subprocess.Popen([exe, "--data-dir", data, "--port", str(port)], cwd=folder, startupinfo=startup)
    units = []
    try:
        window = wait_for(lambda: window_for(process, "SimonAmpsSwitchWindow"))
        edit(window, MAIL_NAME, "existing")
        edit(window, MAIL_PASSWORD, "test-pass")
        click(window, REGISTER)
        database = data / "simon-postoffice.sqlite3"
        wait_for(lambda: scalar(database, "SELECT count(*) FROM users"))
        for _ in range(26):
            unit = Handset(port, register=False)
            units.append(unit)
            unit.send("M D 5551003")
            unit.wait_for("M C 2400")
        registration(units[-1], "5551001")
        failure = bytearray()
        deadline = time.monotonic() + 5
        while b"</html>" not in failure and time.monotonic() < deadline:
            failure.extend(b"".join(bytes.fromhex(s[4:]) for s in units[-1].drain(.1) if s.startswith("M X ")))
        assert b"HTTP/1.0 400" in failure and b"reserved" in failure, failure
        assert scalar(database, "SELECT count(*) FROM users WHERE name='5551001'") == 0
        for i, unit in enumerate(units[:8]):
            registration(unit, f"lab{i}")

        # An old HTTP response or successful PASS must not attach to a new
        # modem call, even when the TCP socket and handset number stay the same.
        reused = units[0]
        for line in ("M H", "M D 5551001",
                     "M X " + b"USER existing\r\nPASS test-pass\r\n".hex(),
                     "M H", "M D 5551001", "M X " + b"LIST\r\n".hex()):
            reused.send(line)
        response = b"".join(bytes.fromhex(s[4:]) for s in reused.drain(1) if s.startswith("M X "))
        assert b"-ERR LOGIN REQUIRED" in response and b"+OK SIMON MAILBOX" not in response, response
        assert b"HTTP/1.0" not in response, response

        for i, unit in enumerate(units[1:]):
            registration(unit, f"batch{i}")
        # Some requests exceed the queue limit. Confirm overload is visible,
        # then close clients and the switch while accepted jobs are in flight.
        deadline = time.monotonic() + 2
        buffers = {unit: b"" for unit in units[1:]}
        while time.monotonic() < deadline and not any(b"503" in buf for buf in buffers.values()):
            for unit in buffers:
                buffers[unit] += b"".join(bytes.fromhex(s[4:]) for s in unit.drain(.001) if s.startswith("M X "))
        assert any(b"503" in buf for buf in buffers.values()), "No overload response"
        for unit in units:
            unit.close()
        units.clear()
        ui.PostMessageW(window, 0x10, 0, 0)
        assert process.wait(5) == 0
        assert scalar(database, "PRAGMA integrity_check") == "ok"
    finally:
        for unit in units:
            unit.close()
        if process.poll() is None:
            process.kill()
            process.wait()
    print("PASS isolated login and HTTP replies after hangup; bounded registration queue and clean shutdown")


def regression(exe, folder):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    first_port, saved_port = available_port(), available_port()
    data = folder / "new post office"
    for first in (True, False):
        args = [exe, "--data-dir", data]
        if first:
            args += ["--port", str(first_port)]
        process = subprocess.Popen(args, cwd=folder, startupinfo=startup)
        unit = None
        try:
            window = wait_for(lambda: window_for(process, "SimonAmpsSwitchWindow"))
            if first:
                ui.SendMessageW(window, 0x111, LANGUAGE_CHINESE, 0)
            title = ctypes.create_unicode_buffer(80)
            ui.SendMessageW(window, 0x000d, len(title), ctypes.addressof(title))
            assert title.value == "Simon AMPS 实验交换机", title.value
            with closing(sqlite3.connect(data / "simon-postoffice.sqlite3")) as db:
                counts = tuple(db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
                               for table in ("users", "letters", "profiles"))
                assert counts == ((0, 0, 0) if first else (2, 1, 1)), counts
                assert db.execute("PRAGMA integrity_check").fetchone() == ("ok",)
            unit = Handset(first_port if first else saved_port, register=False)
            if first:
                for name in ("writer", "reader"):
                    edit(window, MAIL_NAME, name)
                    edit(window, MAIL_PASSWORD, "switch-test")
                    click(window, REGISTER)
                    wait_for(lambda: scalar(data / "simon-postoffice.sqlite3", "SELECT count(*) FROM users WHERE name=?", (name,)))
                edit(window, OPERATOR, "Test Operator")
                click(window, APPLY)
                edit(window, BIND_PORT, str(saved_port))
                click(window, SAVE_LISTENER)
                wait_for(lambda: scalar(data / "simon-postoffice.sqlite3", "SELECT port FROM listener") == saved_port)
            unit.send("N")
            unit.wait_for("S HOME1 6 Test Operator")
            unit.send("M D 5551001")
            unit.wait_for("M C 2400")
            if first:
                commands(unit, ["USER writer", "PASS switch-test", "SEND", "TO: reader",
                                "SUBJECT: persistent", "", "after restart", "."])
            else:
                result = commands(unit, ["USER reader", "PASS switch-test", "RETR 1"])
                assert b"after restart" in result and b"From: writer" in result, result
                with closing(sqlite3.connect(data / "simon-postoffice.sqlite3")) as db:
                    hashes = db.execute("SELECT salt,verifier FROM users").fetchall()
                    assert len(set(hashes)) == 2 and all(len(k) == 32 for _, k in hashes)
            unit.close()
            unit = None
            ui.PostMessageW(window, 0x10, 0, 0)
            assert process.wait(5) == 0
        finally:
            if unit:
                unit.close()
            if process.poll() is None:
                process.kill()
                process.wait()
    print("PASS clean initialization; GUI accounts, profile, listener and delivered mail survive restart")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        regression(args.executable.resolve(), Path(temp))
        pending_accounts(args.executable.resolve(), Path(temp))
