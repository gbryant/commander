#!/usr/bin/env python3
"""Guard: the broker's CHANNELS mirror must match include/channel_ids.h (the authority).

channel_ids.h is the single source of truth for channel ids + roles; the Python broker
keeps a hand-mirrored copy (like i2c_ids.h is mirrored across platforms). If they drift,
the broker sockets the wrong channels / mislabels command sessions and frames effectively
vanish — a silent failure. This parses the C++ table and asserts the two agree, so the
"keep in sync" comment is enforced, not hoped for.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "transport", "channels", "broker"))
import commander_broker as B  # noqa: E402


def parse_channel_ids_h(path):
    text = open(path).read()
    # id constants: inline constexpr uint8_t CH_X = N;
    ids = {m.group(1): int(m.group(2), 0)
           for m in re.finditer(r"constexpr\s+uint8_t\s+(CH_\w+)\s*=\s*(\d+)", text)}
    # table rows: { CH_X, "name", CH_DIR_*, CH_KIND_*, true|false }
    rows = {}
    for m in re.finditer(r'\{\s*(CH_\w+)\s*,\s*"([^"]+)"\s*,\s*CH_DIR_\w+\s*,\s*CH_KIND_\w+\s*,\s*(true|false)\s*\}', text):
        sym, name, cmd = m.group(1), m.group(2), m.group(3) == "true"
        rows[ids[sym]] = (name, cmd)
    return rows


def main():
    rows = parse_channel_ids_h(os.path.join(ROOT, "include", "channel_ids.h"))
    fails = 0

    ok = len(rows) > 0
    print(f"{'PASS' if ok else 'FAIL'} parsed {len(rows)} channel(s) from channel_ids.h")
    fails += not ok

    # same id set
    h_ids, b_ids = set(rows), set(B.CHANNELS)
    ok = h_ids == b_ids
    print(f"{'PASS' if ok else 'FAIL'} broker CHANNELS ids match channel_ids.h ({sorted(b_ids)} vs {sorted(h_ids)})")
    fails += not ok

    # per-channel name + command_session flag agree
    for ch in sorted(h_ids & b_ids):
        h_name, h_cmd = rows[ch]
        b_name, _dir, _kind, b_cmd = B.CHANNELS[ch]
        ok = h_name == b_name and h_cmd == b_cmd
        print(f"{'PASS' if ok else 'FAIL'} ch{ch}: name/role match "
              f"(h={h_name},cmd={h_cmd} | broker={b_name},cmd={b_cmd})")
        fails += not ok

    print("\nALL PASS" if not fails else f"\n{fails} FAILED")
    return fails


if __name__ == "__main__":
    sys.exit(main())
