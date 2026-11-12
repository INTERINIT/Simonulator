"""Black-box switch timing, paging, and TCP-backpressure regressions.

Uses synthetic handsets on an isolated port and leaves real Simon instances
alone. No third-party Python packages are required.
"""

import argparse
import math
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


class Handset:
    def __init__(self, port, receive_buffer=None, register=True):
        self.socket = socket.socket()
        if receive_buffer is not None:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.socket.settimeout(2)
        self.socket.connect(("127.0.0.1", port))
        self.pending = b""
        attached = self.drain(0.08)
        self.number = int(next(line[2:] for line in attached if line.startswith("V ")))
        assert attached == [f"V {self.number}"], "Transport attachment registered an unpowered radio"
        if register:
            self.register()

    def register(self):
        self.send("N")
        _, profile = self.wait_for("S HOME1 6 Simon AMPS")
        assert "D 1 334 6000" in profile, "RF registration did not acquire the control channel"

    def attach_jack(self):
        self.send("M J 1")
        self.jack_number = self.number + 1000
        self.wait_for(f"M V N {self.jack_number}")
        assert 1001 <= self.number <= 1999 and 2001 <= self.jack_number <= 2999

    def send(self, line):
        self.socket.sendall((line + "\n").encode("ascii"))

    def drain(self, duration):
        deadline = time.monotonic() + duration
        lines = []
        while time.monotonic() < deadline:
            self.socket.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                chunk = self.socket.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                raise AssertionError("Switch unexpectedly closed a handset")
            self.pending += chunk
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                lines.append(line.decode("ascii").strip())
        return lines

    def wait_for(self, expected, timeout=1.5):
        start = time.monotonic()
        lines = []
        while time.monotonic() - start < timeout:
            lines.extend(self.drain(0.01))
            if expected in lines:
                return time.monotonic() - start, lines
        raise AssertionError(f"Missing {expected!r}; received {lines[:12]!r}")

    def close(self):
        self.socket.close()


def assert_no_profile(lines):
    assert not any(line.startswith(("V ", "S ")) for line in lines), "Release re-sent registration"
    assert "D 1 334 6000" in lines, "Release did not restore the control channel"


def test_radio_power(port):
    units = []
    try:
        off, on = Handset(port, register=False), Handset(port)
        units.extend((off, on))
        off.send("L")
        _, status = off.wait_for(f"E UNIT {off.number} ATTACHED")
        assert f"E UNIT {on.number} READY" in status
        off.send(f"O {on.number}")
        off.send(f"Q {on.number} 42")
        assert not off.drain(0.15) and not on.drain(0.5), "An unpowered source placed a cellular call/page"
        on.send(f"O {off.number}")
        on.wait_for("B")
        assert not off.drain(0.5), "Unpowered target received a cellular call"
        on.send("H")
        on.drain(0.05)
        on.send(f"Q {off.number} 42")
        assert not off.drain(0.5), "Unpowered target received a pager alert"

        off.register()
        off.send("N")
        assert not off.drain(0.1), "Repeated N re-sent the operator profile"
        on.send(f"O {off.number}")
        off.wait_for(f"R {on.number}")
        off.send("A")
        off.wait_for("C")
        on.wait_for("C")
        off.send("F 0")
        _, release = on.wait_for("H")
        assert_no_profile(release + on.drain(0.05))
        assert not off.drain(0.1), "Powered-off radio received a control-channel/profile reply"
        off.send("L")
        off.wait_for(f"E UNIT {off.number} ATTACHED")
        off.register()
        off.send("F 0")
        assert not off.drain(0.1)
        print("PASS RF power: attachment is not registration; call/page gating, idempotent N, active-call power-off, re-registration")

        # The RJ11/COM2 bearer is electrically independent of RF power.  It
        # remains usable before, during, and after an RF on/off transition.
        off.send("M D 5551000")
        off.wait_for("M C 2400")
        off.send("M X 414243")
        off.wait_for("M X 414243")
        off.register()
        off.send("F 0")
        off.send("M X 444546")
        _, echo = off.wait_for("M X 444546")
        assert not any(line.startswith(("D ", "S ", "V ")) for line in echo)
        off.send("M H")
        off.wait_for("M H")
        off.send("L")
        off.wait_for(f"E UNIT {off.number} ATTACHED")

        # The same service entered via cellular O must terminate on RF off.
        off.register()
        off.send("O 5551000")
        off.wait_for("M C 2400")
        off.send("F 0")
        off.send("M X 474849")
        assert not off.drain(0.15), "Cellular data bearer survived RF power-off"
        print("PASS RF power: independent RJ11 data survives RF cycling; cellular data does not")
    finally:
        for unit in units:
            unit.close()


def send_wireline_digits(unit, digits, held_ms=150):
    rows = (697, 770, 852, 941)
    columns = (1209, 1336, 1477)
    keypad = "123456789*0#"
    for digit in digits:
        position = keypad.index(digit)
        low, high = rows[position // 3], columns[position % 3]
        samples = bytes(
            round(128 + 32 * math.sin(2 * math.pi * low * n / 8000) +
                  32 * math.sin(2 * math.pi * high * n / 8000))
            for n in range(held_ms * 8)
        ) + bytes([128]) * 800
        for offset in range(0, len(samples), 16):
            unit.send("M V Y " + samples[offset:offset + 16].hex())


def assert_no_rf(lines):
    assert not any(line.startswith(("D ", "S ", "V ", "Y ")) or line in ("C", "H", "B")
                   for line in lines), "Wall-jack traffic leaked into the RF protocol"


def test_wireline_voice(port):
    units = []
    try:
        line, mobile, other = Handset(port, register=False), Handset(port), Handset(port)
        units.extend((line, mobile, other))
        line.attach_jack()
        mobile.attach_jack()
        line.send("M L 1")
        dialtone = line.drain(0.1)
        assert any(frame.startswith("M V Y ") for frame in dialtone), "No independent wireline dial tone"
        assert_no_rf(dialtone)
        # This MIN contains repeated zeroes; long held tones must not produce
        # duplicate digits, while the gap must rearm repeated equal digits.
        send_wireline_digits(line, str(mobile.number), held_ms=300)
        assert not mobile.drain(1.4), "Wireline dialled before the interdigit timeout"
        _, assignment = mobile.wait_for(f"R {line.jack_number}", 1.5)
        sat = int(next(frame for frame in assignment if frame.startswith("D ")).split()[-1])
        ringing = line.drain(0.1)
        assert any(frame.startswith("M V Y ") for frame in ringing), "No wireline ringback PCM"
        assert_no_rf(ringing)
        mobile.send("A")
        mobile.wait_for("C")
        _, connected = line.wait_for("M V C")
        assert_no_rf(connected)
        line.send("L")
        line.wait_for(f"E UNIT {line.number} ATTACHED")
        wire_pcm = "808182838485868788898A8B8C8D8E8F"
        rf_pcm = "8F8E8D8C8B8A89888786858483828180"
        line.send("M V Y " + wire_pcm)
        mobile.wait_for("Y " + wire_pcm)
        mobile.send("Y " + rf_pcm)
        _, returned = line.wait_for("M V Y " + rf_pcm)
        assert_no_rf(returned)
        line.register()
        line.send("F 0")
        line.send("M V Y " + wire_pcm)
        mobile.wait_for("Y " + wire_pcm)
        assert not line.drain(0.1), "RF power-off ended the independent wireline call"
        # Only the AMPS leg supervises SAT.  A copper-line caller must not be
        # disconnected after five seconds merely because it sends no RF SAT.
        for _ in range(6):
            mobile.send(f"Z {sat}")
            time.sleep(1)
            assert "M V H" not in line.drain(0.01), "SAT watchdog incorrectly supervised the wireline caller"
            assert "H" not in mobile.drain(0.01)
        mobile.send("H")
        _, released = line.wait_for("M V H")
        assert_no_rf(released + line.drain(0.05))
        mobile.drain(0.05)
        line.send("M L 0")
        assert line.drain(0.05) == ["M V R 0"]
        print("PASS RJ11 voice: off-hook, held/repeated DTMF, interdigit timer, mobile alert, isolated connect/audio/release")

        # Local wireline hang-up releases the cellular peer too.
        line.send("M L 1")
        line.drain(0.05)
        send_wireline_digits(line, str(mobile.number))
        mobile.wait_for(f"R {line.jack_number}", 3)
        mobile.send("A")
        mobile.wait_for("C")
        line.wait_for("M V C")
        line.send("M L 0")
        _, released = mobile.wait_for("H")
        assert_no_profile(released + mobile.drain(0.05))
        assert_no_rf(line.drain(0.05))

        mobile.send("F 0")
        mobile.drain(0.05)
        line.send("M L 1")
        line.drain(0.05)
        send_wireline_digits(line, str(mobile.number))
        _, busy = line.wait_for("M V B", 3)
        assert_no_rf(busy)
        assert not mobile.drain(0.05), "Unregistered mobile received a wireline call"
        line.send("M L 0")
        line.drain(0.05)
        mobile.register()

        # A conflicting off-hook is refused, not allowed to steal an RF call
        # or an existing RJ11 modem session on the same handset.
        mobile.send(f"O {other.number}")
        other.wait_for(f"R {mobile.number}")
        other.send("A")
        other.wait_for("C")
        mobile.wait_for("C")
        mobile.send("M L 1")
        mobile.wait_for("M V B")
        mobile.send("Y " + rf_pcm)
        other.wait_for("Y " + rf_pcm)
        assert not any(frame == "H" for frame in other.drain(0.05))
        mobile.send("H")
        other.wait_for("H")
        mobile.drain(0.05)
        line.send("M D 5551000")
        line.wait_for("M C 2400")
        line.send("M L 1")
        line.wait_for("M V B")
        line.send("M X 414243")
        line.wait_for("M X 414243")
        line.send("M H")
        line.wait_for("M H")
        print("PASS RJ11 voice: local release, RF-off destination rejection, active RF/data conflict safety")
    finally:
        for unit in units:
            unit.close()


def test_incoming_wireline(port):
    units = []
    try:
        mobile, line, second_line = Handset(port), Handset(port, register=False), Handset(port, register=False)
        units.extend((mobile, line, second_line))
        for unit in units:
            unit.attach_jack()
        line_number = line.jack_number
        line.send("M J 0")
        assert line.drain(0.05) == ["M V R 0"]
        line.attach_jack()
        assert line.jack_number == line_number, "Reinsertion changed this session's line number"
        line.attach_jack()  # idempotent: same physical attachment, same number

        # RF -> copper must address the line number and ring its modem RI, not
        # the powered-off RF deck.  Verify the complete 2s-on/4s-off cadence.
        mobile.send(f"O {line_number}")
        _, rings = line.wait_for("M V R 1")
        assert_no_rf(rings)
        start = time.monotonic()
        off_delay, rings = line.wait_for("M V R 0", 2.7)
        assert 1.5 < time.monotonic() - start < 2.7
        assert_no_rf(rings)
        on_delay, rings = line.wait_for("M V R 1", 4.7)
        assert 3.5 < on_delay < 4.7
        assert_no_rf(rings)
        line.send("A")  # wrong bearer must not answer the physical line
        assert not line.drain(0.1)
        line.send("M L 1")
        _, answered = line.wait_for("M V C")
        assert "M V R 0" in answered
        assert_no_rf(answered)
        mobile.wait_for("C")
        pcm = "808182838485868788898A8B8C8D8E8F"
        mobile.send("Y " + pcm)
        line.wait_for("M V Y " + pcm)
        line.send("M V Y " + pcm)
        mobile.wait_for("Y " + pcm)
        line.send("M L 0")
        mobile.wait_for("H")
        assert_no_rf(line.drain(0.05))
        mobile.drain(0.05)
        print(f"PASS incoming RJ11: distinct/stable line number; RF-off RI cadence {off_delay:.2f}s/{on_delay:.2f}s, physical answer and audio")

        # Cancelling before pickup must stop RI, including the scheduled next
        # edge.  Hanging up an on-hook incoming call explicitly rejects it.
        mobile.send(f"O {line_number}")
        line.wait_for("M V R 1")
        mobile.send("H")
        _, cancelled = line.wait_for("M V H")
        assert "M V R 0" in cancelled
        assert_no_rf(cancelled)
        assert not line.drain(2.3), "Cancelled incoming call kept toggling RI"
        mobile.drain(0.05)
        mobile.send(f"O {line_number}")
        line.wait_for("M V R 1")
        line.send("M L 0")
        mobile.wait_for("H")
        assert "M V R 0" in line.drain(0.05)
        mobile.drain(0.05)
        mobile.send(f"O {line_number}")
        line.wait_for("M V R 1")
        line.send("M J 0")
        mobile.wait_for("H")
        unplugged = line.drain(0.05)
        assert "M V R 0" in unplugged
        assert_no_rf(unplugged)
        mobile.drain(0.05)
        mobile.send(f"O {line_number}")
        mobile.wait_for("B")
        assert not line.drain(0.1), "Unplugged jack received ring voltage"
        mobile.send("H")
        mobile.drain(0.05)
        line.attach_jack()
        print("PASS incoming RJ11: caller cancellation, on-hook rejection, ring unplug and absent-line busy")

        # Copper -> copper needs no registration, RF profile, channel or SAT.
        second_line.send("M L 1")
        second_line.drain(0.05)
        send_wireline_digits(second_line, str(line_number))
        _, rings = line.wait_for("M V R 1", 3)
        assert_no_rf(rings)
        line.send("M L 1")
        _, answer_a = line.wait_for("M V C")
        _, answer_b = second_line.wait_for("M V C")
        assert_no_rf(answer_a + answer_b)
        second_line.send("M V Y " + pcm)
        line.wait_for("M V Y " + pcm)
        line.send("M V Y " + pcm)
        second_line.wait_for("M V Y " + pcm)
        mobile.send(f"O {line_number}")
        mobile.wait_for("B")
        mobile.send("H")
        mobile.drain(0.05)
        assert not line.drain(5.6) and not second_line.drain(0.05), "Pure wireline call required SAT or was stolen by a busy caller"
        second_line.send("M J 0")
        line.wait_for("M V H")
        assert_no_rf(second_line.drain(0.05))
        line.send("M L 0")
        line.drain(0.05)
        second_line.attach_jack()
        print("PASS wireline-to-wireline: independent PCM without RF/SAT, real busy, connected unplug release")

        # RF power and the physical jack are independent even when the same
        # handset owns both numbers.  Power-off must not cancel incoming RI.
        line.register()
        mobile.send(f"O {line_number}")
        line.wait_for("M V R 1")
        line.send("F 0")
        assert not line.drain(0.1)
        line.send("M L 1")
        line.wait_for("M V C")
        mobile.wait_for("C")
        line.send("M L 0")
        mobile.wait_for("H")
        line.drain(0.05)
        mobile.drain(0.05)
        line.register()
        mobile.send(f"O {line.number}")
        line.wait_for(f"R {mobile.number}")
        line.send("A")
        line.wait_for("C")
        mobile.wait_for("C")
        line.send("M J 0")
        line.wait_for("M V R 0")
        line.send("Y " + pcm)
        _, voice = mobile.wait_for("Y " + pcm)
        assert "H" not in voice
        line.send("H")
        mobile.wait_for("H")
        line.drain(0.05)
        mobile.drain(0.05)
        line.attach_jack()
        mobile.send(f"O {line_number}")
        line.wait_for("M V R 1")
        line.close()
        mobile.wait_for("H")
        print("PASS endpoint independence: RF off preserves incoming line; jack unplug preserves RF call; socket close releases peer")
    finally:
        for unit in units:
            unit.close()


def test_fax_bearer(source, target):
    # A receiving DTE must see its first bytes while the transmitting DTE
    # still holds the carrier, including when NEXT precedes the next write.
    source.send("M F TX M96")
    source.wait_for("M F READY")
    target.send("M F RX M96")
    target.wait_for("M F START")
    target.send("M F NEXT")
    page = bytes(range(256)) * 128
    blocks = [page[i:i + 64].hex().upper() for i in range(0, len(page), 64)]
    source.send("M F X " + blocks[0])
    target.wait_for("M F X " + blocks[0])

    for block in blocks[1:]:
        source.send("M F X " + block)
    source.send("M F DONE")
    source.wait_for("M F OK")
    target.socket.sendall(b"M F NEXT\n" * len(blocks))
    _, lines = target.wait_for("M F END", timeout=5)
    received = [line[6:] for line in lines if line.startswith("M F X ")]
    assert bytes.fromhex(blocks[0] + "".join(received)) == page
    print(f"PASS streaming Class 1 carrier: {len(page)} bytes with receiver pacing")


def test_dtmf(port):
    caller, peer = Handset(port), Handset(port)
    try:
        caller.send(f"O {peer.number}")
        peer.wait_for(f"R {caller.number}")
        peer.send("A")
        caller.wait_for("C")
        peer.wait_for("C")

        rows = (697, 770, 852, 941)
        cols = (1209, 1336, 1477, 1633)
        keys = "123A456B789C*0#D"
        pcm = bytearray()

        def tone(components):
            for n in range(960):
                pcm.append(round(128 + sum(level * math.sin(2 * math.pi * f * n / 8000 + phase)
                                           for f, level, phase in components)))
            pcm.extend(bytes([128]) * 640)

        # Strong sinusoidal leakage previously made the Android 953 Hz
        # microphone check report '*'. Include every keypad bin alone too.
        for frequency in (577, 953, *rows, *cols):
            tone(((frequency, 112, 0.3),))
        for row in rows:
            for col in cols:
                tone(((row, 25, 0.1), (col, 25, 0.7)))
        # Unequal row/column power on working telephone paths.
        tone(((697, 50, 0.2), (1209, 20, 0.6)))
        tone(((941, 25, 0.3), (1633, 39, 0.9)))
        for offset in range(0, len(pcm), 160):
            caller.send("Y " + pcm[offset:offset + 160].hex())
        events = peer.drain(0.7)
        heard = "".join(line.split()[-1] for line in events if line.startswith("E DTMF "))
        assert heard == keys + "1D", f"Unexpected in-band keypad events: {heard!r}"
        relayed = b"".join(bytes.fromhex(line[2:]) for line in events if line.startswith("Y "))
        assert relayed == pcm, "DTMF detection changed the relayed audio"
        print("PASS single tones including 953 Hz rejected, all 16 DTMF pairs detected, PCM unchanged")
    finally:
        caller.close()
        peer.close()


def test_switch(port):
    units = []
    flood_stop = threading.Event()
    flood_thread = None
    try:
        # Give the lowest-number socket sustained work while calls run in both
        # directions. Bounded fair reads must still process higher sockets.
        load = Handset(port)
        units.append(load)
        low, high = Handset(port), Handset(port)
        units.extend((low, high))

        def flood():
            try:
                while not flood_stop.is_set():
                    load.socket.sendall(b"P RF-ACK\n" * 1024)
                    time.sleep(0.001)
            except OSError:
                pass

        flood_thread = threading.Thread(target=flood, daemon=True)
        flood_thread.start()
        latencies = []
        for source, target in ((low, high), (high, low)):
            source.send(f"O {target.number}")
            delay, _ = target.wait_for(f"R {source.number}")
            assert delay < 0.8, f"Alert delayed {delay:.3f}s under socket load"
            latencies.append(delay)
            source.drain(0.08)
            start = time.monotonic()
            pcm = source.drain(2.0)
            elapsed = time.monotonic() - start
            samples = sum((len(line) - 2) // 2 for line in pcm if line.startswith("Y "))
            rate = samples / elapsed
            assert 7700 < rate < 8300, f"PCM clock drift: {rate:.0f} samples/s"
            target.send("A")
            connect_delay, _ = source.wait_for("C")
            assert connect_delay < 0.2, f"Answer order delayed {connect_delay:.3f}s"
            target.wait_for("C")
            test_fax_bearer(source, target)
            source.send("H")
            _, release = target.wait_for("H")
            release += target.drain(0.05)
            assert_no_profile(release)
            assert_no_profile(source.drain(0.05))
            print(f"PASS {source.number}->{target.number}: alert {delay:.3f}s, connect {connect_delay:.3f}s, PCM {rate:.0f} Hz")
        assert abs(latencies[0] - latencies[1]) < 0.12, "Number order changed alert latency"
        flood_stop.set()
        flood_thread.join(2)

        low.send(f"Q {high.number} 4242")
        high.wait_for("R 5552000 P")
        high.send("A")
        _, page = high.wait_for("H", 5)
        page += high.drain(0.05)
        assert "C" in page and any(line.startswith("Y ") for line in page), "Missing pager bearer"
        assert_no_profile(page)
        print("PASS pager: PCM delivery/release without registration or operator profile")

        # Constrain the receiving TCP window and withhold reads, forcing the
        # switch to keep unsent tails rather than silently discard them.
        slow = Handset(port, receive_buffer=4096)
        units.append(slow)
        slow.send("M D 5551000")
        slow.wait_for("M C 2400")
        payload = bytes(range(256)) * 1200
        for offset in range(0, len(payload), 256):
            slow.send("M X " + payload[offset:offset + 256].hex())
        time.sleep(0.2)
        received = bytearray()
        deadline = time.monotonic() + 12
        while len(received) < len(payload) and time.monotonic() < deadline:
            for line in slow.drain(0.05):
                assert line.startswith("M X "), f"Corrupted TCP line: {line[:64]!r}"
                received.extend(bytes.fromhex(line[4:]))
        assert received == payload, f"Backpressure lost/corrupted data: {len(received)}/{len(payload)}"
        print(f"PASS backpressure: {len(payload)} bytes echoed intact through a 4 KiB receive window")
    finally:
        flood_stop.set()
        for unit in units:
            unit.close()
        if flood_thread is not None:
            flood_thread.join(2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5667)
    parser.add_argument("--executable", type=Path, default=Path(__file__).with_name("Simon_AMPS_Switch.exe"))
    args = parser.parse_args()
    executable = args.executable.resolve()
    # Fail before launching if another task owns this test port.
    with socket.socket() as check:
        check.bind(("127.0.0.1", args.port))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    with tempfile.TemporaryDirectory(prefix="simon-switch-test-") as work:
        process = subprocess.Popen([str(executable), "--port", str(args.port), "--data-dir", "."], cwd=work, startupinfo=startup)
        try:
            time.sleep(0.4)
            assert process.poll() is None, "Switch failed to start"
            test_radio_power(args.port)
            test_wireline_voice(args.port)
            test_incoming_wireline(args.port)
            test_dtmf(args.port)
            test_switch(args.port)
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
