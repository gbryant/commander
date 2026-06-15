#!/usr/bin/env python3
"""irchan.py — channel-bus link shared by the Uno Q IR tools (ir_map.py / ir_lookup.py).

On the Uno Q, IR doesn't come over a serial console — it comes over the commander channel bus.
The broker exposes per-channel Unix sockets under a rundir (default /tmp/commander):

    ch1.sock   IR events  (one canonical line per press, newline-delimited)
    ch0.sock   the console (send `ir recv` here; read its reply) — broker must run with
               `--channels 0,1` so ch0 is exposed as a socket alongside the human console.

This runs on the SBC (Debian) next to the broker. No third-party deps — just Unix sockets.
"""
import socket
import time


class ChannelLink:
    def __init__(self, rundir="/tmp/commander"):
        self.console = self._connect(f"{rundir}/ch0.sock")
        self.events  = self._connect(f"{rundir}/ch1.sock")
        self._cbuf = b""
        self._ebuf = b""

    @staticmethod
    def _connect(path):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
        except (FileNotFoundError, ConnectionRefusedError) as e:
            raise SystemExit(f"can't reach {path}: {e}\n"
                             f"  is the broker running with --channels 0,1? (./install-broker sets that)")
        s.setblocking(False)
        return s

    # ── ch0 console ──────────────────────────────────────────────────────────
    def send(self, command):
        """Send one console command (the broker frames it on ch0)."""
        self.console.sendall(command.encode())

    def wait_console(self, marker, timeout=3.0):
        """Read ch0 output until `marker` appears (or timeout)."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                self._cbuf += self.console.recv(4096)
            except BlockingIOError:
                time.sleep(0.02)
            if marker.encode() in self._cbuf:
                return True
        return False

    def enable_recv(self):
        """Turn on `ir recv` (it toggles, so confirm via the console reply)."""
        self.send("ir recv")
        if not self.wait_console("listening"):
            self.send("ir recv")                       # was on -> we just turned it off; flip back
            if not self.wait_console("listening"):
                print("warning: could not confirm IR receive mode")

    # ── ch1 events ───────────────────────────────────────────────────────────
    def events_lines(self):
        """Yield EVERY IR event line from ch1 (blocking, newline-delimited). Use this when you
        want each press (ir_map.py / ir_lookup.py)."""
        while True:
            try:
                data = self.events.recv(4096)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            if not data:
                return
            self._ebuf += data
            while b"\n" in self._ebuf:
                line, self._ebuf = self._ebuf.split(b"\n", 1)
                yield line.decode("utf-8", "replace").strip()

    def latest_events(self, poll=0.05):
        """Yield the NEWEST buffered ch1 line each cycle (or None when idle for one poll),
        discarding any backlog. A held button retransmits ~20x/s; if a consumer is slower than
        that (e.g. it speaks each press) the queue grows and it replays stale presses seconds
        late. Draining to the latest frame keeps the consumer real-time — it sees the button
        being pressed NOW, and the None idle ticks let it detect a release. For ir_speak.py."""
        while True:
            try:
                while True:                       # drain the whole socket buffer, not one recv
                    data = self.events.recv(4096)
                    if not data:
                        return
                    self._ebuf += data
            except BlockingIOError:
                pass
            latest = None
            while b"\n" in self._ebuf:
                line, self._ebuf = self._ebuf.split(b"\n", 1)
                latest = line.decode("utf-8", "replace").strip()
            yield latest                          # newest line, or None = idle tick
            if latest is None:
                time.sleep(poll)

    def close(self):
        for s in (self.console, self.events):
            try:
                s.close()
            except Exception:
                pass
