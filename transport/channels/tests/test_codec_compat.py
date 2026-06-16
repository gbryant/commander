#!/usr/bin/env python3
"""Codec<->broker byte-compat guard (docs/testing.md).

The MCU's C COBS codec (transport/channels/ChannelCodec.h) and the Python broker's
port of it (transport/channels/broker/commander_broker.py) must stay byte-identical.
If they drift, frames silently vanish — no error, just lost data. This test pins them
together by round-tripping real frames in BOTH directions through the actual C codec
(compiled via codec_harness.cpp) and the actual broker functions:

    C channel_encode()  -> Python Deframer    (encode compat)
    Python frame()      -> C ChannelReader     (decode compat)

It is the permanent home for what used to be a one-off manual cross-check.

Usage: codec_harness binary path is argv[1] (the run.sh wrapper compiles it first).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BROKER_DIR = os.path.join(HERE, "..", "broker")
sys.path.insert(0, os.path.abspath(BROKER_DIR))
import commander_broker as B  # noqa: E402

# Tricky vectors: embedded zeros, empty payload, a >254 run (forces a 0xFF COBS group),
# zeros sprinkled through a long payload, and several distinct channels back-to-back.
VECTORS = [
    (7, bytes([1, 2, 3, 4, 5])),
    (3, bytes([0, 1, 0, 0, 2, 0])),                 # COBS zero stress
    (0, b""),                                        # empty (console-style heartbeat)
    (9, bytes((i % 251) + 1 for i in range(600))),   # 600B, no zero -> 0xFF run
    (9, bytes(0 if i % 7 == 0 else (i & 0xFF) for i in range(600))),  # 600B with zeros
    (1, b"ir:0x20DF10EF"),
    (2, b"sensor=42"),
]


def deframe_all(data: bytes):
    df = B.Deframer()
    out = []
    for ch, payload in df.feed(data):
        out.append((ch, bytes(payload)))
    return out


def main(harness):
    fails = 0

    # ── direction 1: C channel_encode() -> Python Deframer ──────────────────────
    lines = "".join(f"{ch} {pl.hex()}\n" for ch, pl in VECTORS).encode()
    enc = subprocess.run([harness, "encode"], input=lines, capture_output=True, check=True).stdout
    got = deframe_all(enc)
    ok = got == VECTORS
    print(f"{'PASS' if ok else 'FAIL'} C channel_encode -> Python Deframer round-trips "
          f"({len(VECTORS)} frames)")
    if not ok:
        print(f"  expected {[(c, p[:8].hex()) for c, p in VECTORS]}")
        print(f"  got      {[(c, p[:8].hex()) for c, p in got]}")
        fails += 1

    # ── direction 2: Python frame() -> C ChannelReader ──────────────────────────
    pybytes = b"".join(B.frame(ch, pl) for ch, pl in VECTORS)
    out = subprocess.run([harness, "decode"], input=pybytes, capture_output=True, check=True).stdout
    parsed = []
    for line in out.decode().splitlines():
        if not line.strip():
            continue
        ch_s, _, hex_s = line.partition(" ")
        parsed.append((int(ch_s), bytes.fromhex(hex_s) if hex_s else b""))
    ok = parsed == VECTORS
    print(f"{'PASS' if ok else 'FAIL'} Python frame -> C ChannelReader round-trips "
          f"({len(VECTORS)} frames)")
    if not ok:
        print(f"  expected {[(c, p[:8].hex()) for c, p in VECTORS]}")
        print(f"  got      {[(c, p[:8].hex()) for c, p in parsed]}")
        fails += 1

    # ── a single byte flipped in the encoded stream must NOT silently decode wrong:
    # corrupt one payload byte and assert the two sides still AGREE on the result
    # (both recover the same corrupted frame, or both drop it) — i.e. no divergence.
    corrupt = bytearray(enc)
    # flip a byte in the middle of the stream (not a delimiter)
    for i in range(len(corrupt)):
        if corrupt[i] != 0x00:
            corrupt[i] ^= 0x01
            break
    py_corrupt = deframe_all(bytes(corrupt))
    c_out = subprocess.run([harness, "decode"], input=bytes(corrupt), capture_output=True, check=True).stdout
    c_corrupt = []
    for line in c_out.decode().splitlines():
        if not line.strip():
            continue
        ch_s, _, hex_s = line.partition(" ")
        c_corrupt.append((int(ch_s), bytes.fromhex(hex_s) if hex_s else b""))
    ok = py_corrupt == c_corrupt
    print(f"{'PASS' if ok else 'FAIL'} both codecs agree on a corrupted stream (no silent divergence)")
    if not ok:
        fails += 1

    print("\nALL PASS" if not fails else f"\n{fails} FAILED")
    return fails


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: test_codec_compat.py <codec_harness-binary>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
