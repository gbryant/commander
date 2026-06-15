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
    ok = (got == b"0x20DF10EF p3\n"); fails += not ok   # fan-out is newline-delimited
    print(f"{'PASS' if ok else 'FAIL'} ch1 publish (MCU->SBC) reaches socket client newline-framed: {got!r}")

    # 2) ch1.sock client write -> framed to the MCU on ch1
    cli.sendall(b"hello-mcu")
    got = read_frames(m)
    ok = any(ch == 1 and pay == b"hello-mcu" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} socket client write (SBC->MCU) framed on ch1: {got}")

    # 3) typing a line into the console PTY -> one stripped command frame to the MCU on ch0
    cfd = os.open(os.readlink(os.path.join(rundir, "console")), os.O_RDWR | os.O_NONBLOCK)
    os.write(cfd, b"ir recv\r\n")               # CR/LF terminates the command
    got = read_frames(m)
    ok = any(ch == 0 and pay == b"ir recv" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} console line framed as one ch0 command (newline stripped): {got}")

    # 3b) per-keystroke input still yields one frame per line, not per key
    for ch_byte in b"version\n":
        os.write(cfd, bytes([ch_byte]))
        time.sleep(0.01)
    got = read_frames(m)
    ok = any(ch == 0 and pay == b"version" for ch, pay in got) and len(got) == 1; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} char-by-char input coalesced into one ch0 command: {got}")

    # 4) MCU frames ch0 output -> shows up on the console PTY
    os.write(m, B.frame(0, b"commander v1\r\n"))
    out = b""
    end = time.time() + 1.0
    while time.time() < end and b"commander v1" not in out:
        try: out += os.read(cfd, 200)
        except BlockingIOError: time.sleep(0.01)
    ok = b"commander v1" in out; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} ch0 frame from MCU appears on console PTY: {out!r}")

    fails += device_console()
    fails += ch0_socket()
    fails += console_hangup()
    print("\nALL PASS" if not fails else f"\n{fails} FAILED")
    return fails


def console_hangup():
    """A hangup on the ch0 console fd (the --console /dev/ttyGS0 gadget's Mac host detaching —
    e.g. a `bum` monitor closing) must be DETECTED and the console reopened, not spun on. A
    HUP'd fd reports readable forever; an empty read that wasn't treated as a hangup would peg
    the select loop at 100% CPU. Regression test for that."""
    fails = 0

    # Console.on_input() returns False on a hangup (empty read on a readable fd)...
    r, w = os.pipe()
    os.close(w)                                  # writer gone -> read() returns b"" (EOF/HUP)
    ok = (B.Console(None, r, echo=True).on_input() is False); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} Console.on_input() returns False on a hangup (empty read)")
    os.close(r)

    # ...and True while the fd is live (input that doesn't complete a line touches no link)
    r2, w2 = os.pipe(); os.set_blocking(r2, False); os.write(w2, b"x")
    ok = (B.Console(None, r2, echo=False).on_input() is True); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} Console.on_input() returns True on a live fd")
    os.close(r2); os.close(w2)

    # The broker reopens after a hangup and ch0 still flows (driven synchronously to avoid
    # racing a background select loop against the reopen's selector mutation).
    lm, ls = os.openpty()
    for fd in (lm, ls): tty.setraw(fd); os.set_blocking(fd, False)

    class FakeLink:
        port, baudrate = "pty", 115200
        def fileno(self): return ls
        def read(self, n):
            try: return os.read(ls, n)
            except BlockingIOError: return b""
        def write(self, b): return os.write(ls, b)

    rundir = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cmdr-brk-test-hup")
    broker = B.Broker(FakeLink(), rundir, [1], log=False)   # not started — pump it by hand
    link = os.path.join(rundir, "console")
    old_pty = os.readlink(link)                  # fd numbers get recycled — compare the PTY name
    broker._reopen_console()
    ok = (os.readlink(link) != old_pty); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} _reopen_console() allocates a fresh console PTY")

    cfd = os.open(os.readlink(link), os.O_RDWR | os.O_NONBLOCK)
    os.write(lm, B.frame(0, b"after-reopen")); time.sleep(0.05)
    broker._on_link()
    out = b""
    end = time.time() + 1.0
    while time.time() < end and b"after-reopen" not in out:
        try: out += os.read(cfd, 200)
        except BlockingIOError: time.sleep(0.01)
    ok = b"after-reopen" in out; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} ch0 still flows to the reopened console: {out!r}")
    return fails


def ch0_socket():
    """`--channels 0` exposes the console as a socket too (programmatic command access for
    local SBC tools, e.g. the IR mapper sending `ir recv`). Both directions on ch0.sock."""
    fails = 0
    lm, ls = os.openpty()                       # MCU link
    for fd in (lm, ls):
        tty.setraw(fd); os.set_blocking(fd, False)

    class FakeLink:
        port, baudrate = "pty", 115200
        def fileno(self): return ls
        def read(self, n):
            try: return os.read(ls, n)
            except BlockingIOError: return b""
        def write(self, b): return os.write(ls, b)

    rundir = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cmdr-brk-test-ch0")
    broker = B.Broker(FakeLink(), rundir, [0, 1], log=False)   # ch0 exposed as a socket
    threading.Thread(target=broker.run, daemon=True).start()
    time.sleep(0.2)

    cli = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    cli.connect(os.path.join(rundir, "ch0.sock"))
    cli.settimeout(1.0)
    time.sleep(0.1)

    # client -> MCU: a console command frames on ch0
    cli.sendall(b"ir recv")
    got = read_frames(lm)
    ok = any(ch == 0 and pay == b"ir recv" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} ch0.sock client command frames to MCU on ch0: {got}")

    # MCU -> client: ch0 output fans out to the ch0.sock client (newline-delimited)
    os.write(lm, B.frame(0, b"listening... (ir recv to stop)"))
    try: reply = cli.recv(200)
    except socket.timeout: reply = b""
    ok = reply == b"listening... (ir recv to stop)\n"; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} ch0 MCU output fans out to the ch0.sock client: {reply!r}")
    return fails


def device_console():
    """--console <dev> path: an interactive endpoint (stand-in for /dev/ttyGS0 -> the Mac).
    The broker must echo + line-edit and frame whole commands on ch0."""
    fails = 0
    lm, ls = os.openpty()                       # MCU link
    cm, cs = os.openpty()                        # the "Mac" console; broker opens the slave
    cdev = os.ttyname(cs); os.close(cs)          # hand the broker the device path, keep master
    for fd in (lm, ls, cm):
        tty.setraw(fd); os.set_blocking(fd, False)

    class FakeLink:
        port, baudrate = "pty", 115200
        def fileno(self): return ls
        def read(self, n):
            try: return os.read(ls, n)
            except BlockingIOError: return b""
        def write(self, b): return os.write(ls, b)

    rundir = os.path.join(os.environ.get("TMPDIR", "/tmp"), "cmdr-brk-test-dev")
    broker = B.Broker(FakeLink(), rundir, [1], log=False, console_dev=cdev)
    threading.Thread(target=broker.run, daemon=True).start()
    time.sleep(0.2)

    # an interactive endpoint gets a "> " prompt at startup
    init = b""
    end = time.time() + 1.0
    while time.time() < end and b"> " not in init:
        try: init += os.read(cm, 200)
        except BlockingIOError: time.sleep(0.01)
    ok = init == b"> "; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} device console prints an initial prompt: {init!r}")

    # type "help\r" a key at a time -> broker echoes it, frames "help" on ch0, re-prompts
    for ch_byte in b"help\r":
        os.write(cm, bytes([ch_byte])); time.sleep(0.01)
    echo = b""
    end = time.time() + 1.0
    while time.time() < end and echo.count(b"> ") < 1:
        try: echo += os.read(cm, 200)
        except BlockingIOError: time.sleep(0.02)
    ok = b"help" in echo; fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} device console echoes typed input back to the Mac: {echo!r}")
    ok = b"> " in echo; fails += not ok       # the next prompt after the command settles
    print(f"{'PASS' if ok else 'FAIL'} device console re-prompts after the command: {echo!r}")

    got = read_frames(lm)
    ok = any(ch == 0 and pay == b"help" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} device console frames the command on ch0: {got}")

    # backspace editing: "worl" <bs> "d\r" -> "word"
    for ch_byte in b"worl\x7fd\r":
        os.write(cm, bytes([ch_byte])); time.sleep(0.01)
    got = read_frames(lm)
    ok = any(ch == 0 and pay == b"word" for ch, pay in got); fails += not ok
    print(f"{'PASS' if ok else 'FAIL'} device console backspace edits the line: {got}")
    return fails


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
