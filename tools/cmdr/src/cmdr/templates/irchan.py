#!/usr/bin/env python3
"""irchan.py — channel-bus link shared by the Uno Q IR tools (ir_map / ir_lookup / ir_speak).

On the Uno Q, IR presses come over the commander channel bus, not a serial console. The
broker exposes per-channel Unix sockets under a rundir (default /tmp/commander); these tools
only need:

    ch1.sock   IR events — one canonical line per press, newline-delimited (subscribe)

The tools are **pure ch1 subscribers** — they don't drive the console. The IR receiver is
started at boot via autostart (one-time setup on the board:  `cmdr autostart add "ir recv"`),
so a fresh board streams presses with no command sent and the human console (ch0) stays
private. (Before, the tools reached through ch0 to send `ir recv`, which meant sharing the
human console and running the broker with `--channels 0,1`; autostart removed that need.)

Runs on the SBC (Debian) next to the broker. No third-party deps — just Unix sockets.
"""
import socket
import time


class ChannelLink:
    def __init__(self, rundir="/tmp/commander"):
        self.rundir = rundir
        self.events = self._connect(f"{rundir}/ch1.sock")
        self._ebuf = b""

    @staticmethod
    def _connect(path):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
        except (FileNotFoundError, ConnectionRefusedError) as e:
            raise SystemExit(f"can't reach {path}: {e}\n"
                             f"  is the broker running? (./install-broker sets it up; ch1 is "
                             f"exposed by default)")
        s.setblocking(False)
        return s

    def hint(self):
        """One-line nudge for the common 'I see no presses' case — the stream isn't on."""
        return ('listening on ch1 — if no presses appear, start the IR stream on the board:\n'
                '  cmdr autostart add "ir recv"   (one-time; then re-flash)')

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
        try:
            self.events.close()
        except Exception:
            pass
