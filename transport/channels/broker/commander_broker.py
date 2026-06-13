#!/usr/bin/env python3
"""commander channel-bus broker (SBC side).

Owns the framed MCU link (a serial port, e.g. /dev/ttyHS1 on the Arduino Uno Q) and
demultiplexes the COBS channel bus (see docs/commander-channels-design.md) out to local
consumers — the half that can ONLY live off-MCU once output is tagged at the source:

  * ch0 (console)  <->  a PTY  (symlinked at <rundir>/console)
        So the human shell still works: `screen $(readlink <rundir>/console)` or
        `picocom $(readlink ...)`. Bytes you type are framed onto ch0; commander's
        framed ch0 output is written back to the PTY. (Editing/echo is the terminal's
        job — commander sends whole lines, no per-key echo, by design.)

  * chN (N>0)      <->  a Unix stream socket  (<rundir>/chN.sock)
        Every process connected to chN receives that channel's inbound frames (fan-out),
        and anything it writes is framed to the MCU on chN. So many Debian processes —
        perception, teleop, logger, autonomy — each get their own tagged stream over the
        one wire. e.g.  socat - UNIX-CONNECT:<rundir>/ch1.sock   (subscribe to ir events)

No third-party dependencies — opens the serial link with termios directly (a stripped
Debian like the Uno Q has no pip/pyserial).

Note: this process must be the SOLE owner of the link. On the Uno Q, stop the USB-CDC
bridge first (it also holds ttyHS1):  sudo systemctl stop commander-bridge.service
"""
import argparse
import os
import selectors
import socket
import termios
import time
import tty

DELIM = 0x00


# ── COBS framing — a direct port of transport/channels/ChannelCodec.h ────────────────
def cobs_encode(data: bytes) -> bytearray:
    out = bytearray()
    code_pos = len(out)
    out.append(0)            # reserve first code byte
    code = 1
    for b in data:
        if b == 0:
            out[code_pos] = code
            code_pos = len(out); out.append(0); code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_pos] = code
                code_pos = len(out); out.append(0); code = 1
    out[code_pos] = code
    return out


def cobs_decode(data: bytes):
    out = bytearray()
    rd, n = 0, len(data)
    while rd < n:
        code = data[rd]; rd += 1
        if code == 0:
            return None                      # no zeros allowed inside COBS data
        for _ in range(1, code):
            if rd >= n:
                return None                  # truncated run
            out.append(data[rd]); rd += 1
        if code != 0xFF and rd < n:
            out.append(0)                    # implicit zero
    return bytes(out)


def frame(channel: int, payload: bytes) -> bytes:
    """COBS([channel|payload]) + delimiter — matches channel_encode()."""
    enc = cobs_encode(bytes([channel]) + payload)
    enc.append(DELIM)
    return bytes(enc)


class Deframer:
    """Feed bytes; yields (channel, payload) for each complete frame."""
    def __init__(self):
        self._raw = bytearray()

    def feed(self, data: bytes):
        for b in data:
            if b == DELIM:
                if self._raw:
                    dec = cobs_decode(bytes(self._raw))
                    self._raw.clear()
                    if dec:
                        yield dec[0], dec[1:]
            else:
                self._raw.append(b)


# ── MCU serial link ────────────────────────────────────────────────────────────────
class SerialLink:
    """Raw tty link to the MCU over termios — no pyserial dependency (a stripped Debian,
    e.g. the Uno Q, has no pip/pyserial). Exposes just what the Broker uses."""
    _BAUDS = {b: getattr(termios, f"B{b}", None)
              for b in (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)}

    def __init__(self, port, baud):
        self.port = port
        self.baudrate = baud
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)                          # 8N1, no echo/translation — binary-clean
        speed = self._BAUDS.get(baud)
        if speed is not None:
            a = termios.tcgetattr(self.fd)
            a[4] = a[5] = speed                      # ispeed, ospeed
            termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def fileno(self):
        return self.fd

    def read(self, n):
        try:
            return os.read(self.fd, n)
        except BlockingIOError:
            return b""

    def write(self, b):
        return os.write(self.fd, b)


# ── ch0 console endpoint ───────────────────────────────────────────────────────────
class Console:
    """The ch0 console as a line editor over one fd. The MCU no longer echoes (the bus is
    line-oriented — it dispatches a whole command line per frame), so for an INTERACTIVE
    endpoint the broker supplies echo + minimal editing. That's what lets the Uno Q's
    USB-CDC gadget (/dev/ttyGS0) still feel like the old serial console to a Mac `screen`,
    so the bus and the Mac console coexist. A PTY endpoint leaves echo off and lets the
    local terminal's cooked mode handle it.

    On an interactive (echo) endpoint it also prints a `> ` prompt: once at start, then after
    each command's reply settles. "Settles" = a brief quiet gap, since one command can produce
    several ch0 frames (a long `help` flushes more than one) — a per-frame prompt would wedge
    `> ` mid-output, so we wait for the output to go idle before re-prompting."""
    PROMPT_IDLE = 0.05                            # seconds of ch0 quiet before re-prompting

    def __init__(self, link, fd, echo, prompt=b"> "):
        self.link = link
        self.fd = fd
        self.echo = echo
        self.prompt = prompt if echo else b""     # only an interactive endpoint gets a prompt
        self.line = bytearray()
        self.pending = False                      # a prompt is owed once output settles
        self.last = 0.0                           # time of the last console write

    def start(self):
        if self.prompt:
            os.write(self.fd, self.prompt)

    def on_input(self):
        try:
            data = os.read(self.fd, 4096)
        except OSError:
            return
        out = bytearray()
        for b in data:
            if b in (0x0D, 0x0A):                 # CR/LF -> dispatch one command
                if self.echo:
                    out += b"\r\n"
                if self.line:
                    self.link.write(frame(0, bytes(self.line)))
                    self.line.clear()
                    self.pending = bool(self.prompt)   # owe a prompt after the reply
            elif b in (0x7F, 0x08):               # backspace / DEL
                if self.line:
                    self.line.pop()
                    if self.echo:
                        out += b"\b \b"
            elif b >= 0x20:
                self.line.append(b)
                if self.echo:
                    out.append(b)
        if out:
            os.write(self.fd, bytes(out))
            self.last = time.time()

    def write(self, payload):                     # MCU ch0 output -> the console
        os.write(self.fd, payload)
        self.last = time.time()                   # defer any owed prompt until output settles

    def prompt_timeout(self):                     # wake the select loop to re-prompt
        return self.PROMPT_IDLE if self.pending else None

    def maybe_prompt(self):
        if self.pending and time.time() - self.last >= self.PROMPT_IDLE:
            os.write(self.fd, self.prompt)
            self.pending = False


# ── Broker ───────────────────────────────────────────────────────────────────────
class Broker:
    def __init__(self, link, rundir, channels, log, console_dev=None):
        self.link = link
        self.rundir = rundir
        self.log = log
        self.sel = selectors.DefaultSelector()
        self.deframer = Deframer()
        self.console = None              # ch0 line editor (over a PTY or a device fd)
        self.servers = {}                # channel -> listening socket
        self.clients = {}                # channel -> set(connected sockets)

        os.makedirs(rundir, exist_ok=True)
        self.sel.register(link, selectors.EVENT_READ, ("link", None))
        self._open_console(console_dev)
        for ch in channels:
            self._open_channel(ch)

    # ch0 console: a device (e.g. /dev/ttyGS0 -> the Mac) or, by default, a local PTY -------
    def _open_console(self, dev):
        if dev:
            fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                tty.setraw(fd)           # raw: broker owns echo/editing + binary-clean frames
            except termios.error:
                pass                     # not a tty (e.g. a plain fd under test) — fine
            self.console = Console(self.link, fd, echo=True)
            self.sel.register(fd, selectors.EVENT_READ, ("console", None))
            self.console.start()                 # initial prompt
            print(f"[broker] ch0 console -> {dev} (interactive, echo on)", flush=True)
            return
        master, slave = os.openpty()
        os.set_blocking(master, False)
        # Raw the slave so its line discipline doesn't ECHO what we write to the master back
        # as input — otherwise every ch0 reply we display loops straight back to the MCU as a
        # bogus command. Keep the slave fd open so the raw setting persists for later openers
        # (screen/echo). We supply our own echo/editing, so a raw slave loses nothing.
        tty.setraw(slave)
        self._console_slave = slave
        path = os.path.join(self.rundir, "console")
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        os.symlink(os.ttyname(slave), path)
        self.console = Console(self.link, master, echo=True)
        self.sel.register(master, selectors.EVENT_READ, ("console", None))
        self.console.start()                         # initial prompt
        print(f"[broker] ch0 console -> {path} (screen $(readlink {path}))", flush=True)

    # chN unix socket server ---------------------------------------------------------
    def _open_channel(self, ch):
        path = os.path.join(self.rundir, f"ch{ch}.sock")
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(8)
        srv.setblocking(False)
        self.servers[ch] = srv
        self.clients[ch] = set()
        self.sel.register(srv, selectors.EVENT_READ, ("accept", ch))
        print(f"[broker] ch{ch} -> {path}", flush=True)

    # event loop ---------------------------------------------------------------------
    def run(self):
        print(f"[broker] up on {self.link.port} @ {self.link.baudrate}", flush=True)
        while True:
            for key, _ in self.sel.select(self.console.prompt_timeout()):
                kind, ch = key.data
                if kind == "link":
                    self._on_link()
                elif kind == "console":
                    self.console.on_input()
                elif kind == "accept":
                    self._on_accept(ch)
                elif kind == "client":
                    self._on_client(key.fileobj, ch)
            self.console.maybe_prompt()

    def _on_link(self):
        data = self.link.read(4096)
        if not data:
            return
        for ch, payload in self.deframer.feed(data):
            if self.log:
                print(f"[ch{ch}] {payload!r}", flush=True)
            if ch == 0:
                self.console.write(payload)
            else:
                self._fanout(ch, payload)

    def _on_accept(self, ch):
        conn, _ = self.servers[ch].accept()
        conn.setblocking(False)
        self.clients[ch].add(conn)
        self.sel.register(conn, selectors.EVENT_READ, ("client", ch))

    def _on_client(self, conn, ch):
        try:
            data = conn.recv(4096)
        except OSError:
            data = b""
        if not data:
            self.sel.unregister(conn)
            self.clients[ch].discard(conn)
            conn.close()
            return
        self.link.write(frame(ch, data))

    def _fanout(self, ch, payload):
        for conn in list(self.clients.get(ch, ())):
            try:
                conn.sendall(payload)
            except OSError:
                self.sel.unregister(conn)
                self.clients[ch].discard(conn)
                conn.close()


def parse_channels(s):
    return [int(x) for x in s.split(",") if x.strip()] if s else []


def main():
    ap = argparse.ArgumentParser(description="commander channel-bus broker (SBC side)")
    ap.add_argument("-p", "--port", default="/dev/ttyHS1", help="MCU serial link (default ttyHS1)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-r", "--rundir", default="/tmp/commander", help="dir for console PTY + chN.sock")
    ap.add_argument("-c", "--channels", default="1,2", help="non-console channels to expose, comma list")
    ap.add_argument("-C", "--console", default=None,
                    help="bridge ch0 to this device instead of a local PTY — e.g. /dev/ttyGS0 "
                         "(the Uno Q USB-CDC gadget), so the Mac serial console keeps working "
                         "alongside the channels. Broker supplies echo + line editing.")
    ap.add_argument("--log", action="store_true", help="print every inbound frame to stdout")
    args = ap.parse_args()

    link = SerialLink(args.port, args.baud)
    Broker(link, args.rundir, parse_channels(args.channels), args.log,
           console_dev=args.console).run()


if __name__ == "__main__":
    main()
