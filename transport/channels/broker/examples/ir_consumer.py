#!/usr/bin/env python3
"""Example consumer: print IR button presses from the commander channel bus.

The broker (commander_broker.py) fans each channel out to a Unix socket,
newline-delimited. ch1 carries IR events in commander's *canonical* format —
the same one IRremote's printIRResultShort emits and the cmdr IR tools
(irmap.py / irlookup.py) parse, so this works for ANY commander board (Arduino
IRremote, the Uno Q Zephyr NEC/Sony decoder, ...):

    Protocol=Sony Address=0x93a, Command=0x15, Raw-Data=0x49d15, 20 bits

Usage:
    python3 ir_consumer.py [SOCKET]          # default /tmp/commander/ch1.sock

First enable IR receive on the MCU console (ch0): type `ir recv`.
No dependencies (stdlib only).
"""
import re
import socket
import sys

# Same pattern as bin/irmap.py — the canonical commander IR line.
IR_RE = re.compile(
    r'Protocol=(\w+)\s+Address=(0x[0-9A-Fa-f]+),\s+'
    r'Command=(0x[0-9A-Fa-f]+),\s+Raw-Data=(0x[0-9A-Fa-f]+),\s+(\d+)\s+bits'
)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/commander/ch1.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    print(f"[ir] connected to {path} — press remote buttons (enable with `ir recv` on ch0)", flush=True)

    buf = b""
    last = None
    while True:
        data = sock.recv(4096)
        if not data:
            print("[ir] channel closed by broker")
            return
        buf += data
        while b"\n" in buf:                      # the broker newline-delimits each event
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            m = IR_RE.search(line)
            if not m:
                print(f"[ir] (unparsed) {line!r}", flush=True)
                continue
            proto, addr, cmd, rawdata, bits = m.groups()
            key = (proto, addr, cmd)             # maps key on protocol+address+command
            tag = "  (repeat)" if key == last else ""
            last = key
            print(f"{proto:5}  addr={addr:6}  cmd={cmd:6}  {bits}-bit{tag}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, ConnectionError, FileNotFoundError) as e:
        print(f"[ir] {e}" if str(e) else "")
