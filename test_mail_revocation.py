"""Delete an active mailbox via the manager, then register the same name/password."""
import argparse
from binascii import crc_hqx
from pathlib import Path
import subprocess
import tempfile


from test_persistence import (MAIL_NAME, MAIL_PASSWORD, REGISTER, available_port, click, edit,
                              mailbox_count, open_mailboxes, scalar, ui, wait_for, window_for)
from test_transport import Handset

def modem(phone, data):
    phone.send("M X " + data.hex())

def text_reply(phone, expected):
    data = bytearray()
    def ready():
        for line in phone.drain(.03):
            if line.startswith("M X "):
                data.extend(bytes.fromhex(line[4:]))
        return expected in data
    wait_for(ready)

def nft(control, data):
    def crc(value):
        return (crc_hqx(value, 65535) ^ 65535).to_bytes(2, "big")
    header = bytes([len(data), control]) + data[:1]
    return b"\x02" + header + crc(header) + (data[1:] + crc(data[1:]) if len(data) > 1 else b"")

def native_login(phone):
    modem(phone, nft(7, b"\xff\xaa\x55\x11"))
    text_reply(phone, nft(0x80, b"\x0f"))
    modem(phone, nft(8, b"\x80"))
    text_reply(phone, nft(9, b"\x81"))
    modem(phone, nft(0x11, b"\x06alpha\x00"))
    text_reply(phone, nft(0x12, b"\x8b"))
    modem(phone, nft(0x1a, b"\x05pass\x00"))
    text_reply(phone, nft(0x1b, b"\x82"))
    modem(phone, nft(0x43, b"\x00"))
    text_reply(phone, nft(0x23, b"\x08" + b"5551001\x00"))
    modem(phone, nft(0x24, b"\x82"))
    text_reply(phone, nft(0x44, b"\x00"))

def regression(exe, data):
    port = available_port()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    proc = subprocess.Popen([exe, "--data-dir", data, "--port", str(port)], cwd=data, startupinfo=startup)
    phones = []
    try:
        main = wait_for(lambda: window_for(proc, "SimonAmpsSwitchWindow"))
        manager = open_mailboxes(proc, main)
        listing = ui.FindWindowExW(manager, None, "SysListView32", None)
        def create(name, count):
            edit(manager, MAIL_NAME, name)
            edit(manager, MAIL_PASSWORD, "pass")
            click(manager, REGISTER)
            wait_for(lambda: mailbox_count(manager) == count)
        create("alpha", 1)
        create("beta", 2)
        for _ in range(3):
            phone = Handset(port, register=False)
            phones.append(phone)
            phone.send("M D 5551001")
            phone.wait_for("M C 2400")
        text, native, sender = phones
        modem(text, b"USER alpha\r\nPASS pass\r\n")
        text_reply(text, b"+OK SIMON MAILBOX")
        native_login(native)
        modem(sender, b"USER beta\r\nPASS pass\r\nSEND\r\nTO: alpha\r\n")
        text_reply(sender, b"+OK END WITH")

        ui.SendMessageW(listing, 0x100, 0x24, 0)  # select first row with Home
        ui.SendMessageW(listing, 0x101, 0x24, 0)
        button = ui.GetDlgItem(manager, 135)  # IDC_DELETE_MAILBOX
        assert button
        ui.PostMessageW(button, 0x00f5, 0, 0)  # BM_CLICK, confirmation is modal
        confirm = wait_for(lambda: window_for(proc, "#32770"))
        click(confirm, 6)  # IDYES
        wait_for(lambda: mailbox_count(manager) == 1)
        for phone in (text, native):
            phone.wait_for("M H")
        create("alpha", 2)
        modem(sender, b"SUBJECT: stale\r\n\r\nbody\r\n.\r\n")
        text_reply(sender, b"-ERR NO DELIVERY")
        assert scalar(data/"simon-postoffice.sqlite3", "SELECT COUNT(*) FROM letters") == 0
        text.send("M D 5551001")
        text.wait_for("M C 2400")
        modem(text, b"USER alpha\r\nPASS pass\r\nLIST\r\n")
        text_reply(text, b"+OK 0\r\n.")
    finally:
        for phone in phones:
            phone.close()
        if proc.poll() is None:
            ui.PostMessageW(main, 0x10, 0, 0)
            try: proc.wait(5)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    print("PASS original NFT and text logins revoked; pending delivery rejected after same-name re-registration")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    with tempfile.TemporaryDirectory() as folder:
        regression(parser.parse_args().executable.resolve(), Path(folder))
