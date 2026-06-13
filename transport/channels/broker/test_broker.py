#!/usr/bin/env python3
"""Host test for the broker's demux/fan-out plumbing (no hardware, no pyserial).

Runs the real Broker against a pseudo-terminal standing in for the MCU link, and checks
both directions on ch0 (console PTY) and ch1 (unix socket): MCU->consumer fan-out and
consumer->MCU framing. This covers the selector loop / PTY bridge / socket fan-out that
the codec cross-check (run.sh) doesn't touch.
"""
import os
import socket
import sys
import termios
import threading
import time
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import commander_broker as B


def read_frames(fd, timeout=1.0):
    """Drain fd until the Deframer yields >=1 frame or timeout; return [(ch,payload)]."""
    df = B.Deframer()
    out = []
    end = time.time() + timeout
    while time.time() < end and not out:
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            data = b""
        if data:
            out += list(df.feed(data))
        else:
            time.sleep(0.01)
    return out


def main():
    m, s = os.openpty()                 # m = "MCU" side, s = broker's link
    for fd in (m, s):                   # raw mode: a real serial link is raw, not cooked —
        tty.setraw(fd)                  # else canonical buffering/special-char processing
        os.set_blocking(fd, False)      # would mangle binary COBS frames (no newlines)

    class FakeLink:                     # what Broker needs of a serial port
        port, baudrate = "pty", 115200
        def fileno(self): return s
        def read(self, n):
            try: return os.read(s, n)
            except BlockingIOError: return b""
        def write(self, b): return os.write(s, b)

    rundir = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cmdr-brk-test")
    broker = B.Broker(FakeLink(), rundir, [1], log=False)
    threading.Thread(target=broker.run, daemon=True).start()
    time.sleep(0.2)

    fails = 0

    # 1) MCU publishes on ch1 -> a connected ch1.sock client receives the payload
    cli = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    cli.connect(os.path.join(rundir, "ch1.sock"))
    cli.settimeout(1.0)
    time.sleep(0.1)
    os.write(m, B.frame(1, b"0x20DF10EF p3"))
    try: got = cli.recv(100)
    except socket.timeout: got = b""
    ok = (got == b"0x20DF10EF p3"); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} ch1 publish (MCU->SBC) reaches socket client: {got!r}")

    # 2) ch1.sock client write -> framed to the MCU on ch1
    cli.sendall(b"hello-mcu")
    got = read_frames(m)
    ok = any(ch == 1 and pay == b"hello-mcu" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} socket client write (SBC->MCU) framed on ch1: {got}")

    # 3) typing into the console PTY -> framed to the MCU on ch0
    cfd = os.open(os.readlink(os.path.join(rundir, "console")), os.O_RDWR | os.O_NONBLOCK)
    os.write(cfd, b"help")
    got = read_frames(m)
    ok = any(ch == 0 and pay == b"help" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} console PTY input framed to MCU on ch0: {got}")

    # 4) MCU frames ch0 output -> shows up on the console PTY
    os.write(m, B.frame(0, b"commander v1\r\n"))
    out = b""
    end = time.time() + 1.0
    while time.time() < end and b"commander v1" not in out:
        try: out += os.read(cfd, 200)
        except BlockingIOError: time.sleep(0.01)
    ok = b"commander v1" in out; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} ch0 frame from MCU appears on console PTY: {out!r}")

    print("\nALL PASS" if not fails else f"\n{fails} FAILED")
    return fails


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
