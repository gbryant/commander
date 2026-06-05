#!/usr/bin/env python3
"""Push OTA firmware to a running commander device via Telnet.

Environment variables (set by the calling shell script):
  OTA_HOST  — hostname or IP of the device  (e.g. pico.local)
  OTA_URL   — full HTTP URL of the fota image
               (e.g. http://192.168.1.x:8000/app_fota_image.bin)

Confirms the OTA actually took: it reads the device's reported build number
(`version`) afterward and checks it against ./.build_number (the image just
built); if that file is absent it falls back to "did the build number change?".
A telnet reconnect alone is NOT treated as success — a failed download leaves the
device running the OLD firmware with telnet still up, which used to read as a
false success. Exits 0 only when the new build is confirmed running; 1 otherwise.
"""
import os, re, socket, time, sys


def strip_iac(data: bytes) -> bytes:
    """Strip RFC 854 IAC telnet negotiation bytes."""
    out, i = bytearray(), 0
    while i < len(data):
        if data[i] == 0xFF and i + 1 < len(data):
            i += 3 if data[i + 1] >= 0xFB else 2
        else:
            out.append(data[i])
            i += 1
    return bytes(out)


def recv_until_prompt(s, timeout: float = 15.0) -> str:
    """Read until the shell prompt ('> '), returning the decoded text."""
    s.settimeout(timeout)
    raw = b""
    while True:
        try:
            chunk = s.recv(256)
        except OSError:
            break
        if not chunk:
            break
        raw += chunk
        if strip_iac(raw).rstrip().endswith(b">"):
            break
    return strip_iac(raw).decode("utf-8", errors="ignore")


def parse_build(text: str):
    """Pull the integer build number out of a `version` reply, or None."""
    m = re.search(r"build\s+(\d+)", text)
    return int(m.group(1)) if m else None


def run_command(host: str, cmd: str, timeout: float = 5) -> "str | None":
    """Connect, run one shell command, return the device's reply text (or None)."""
    try:
        s = socket.create_connection((host, 23), timeout=timeout)
        recv_until_prompt(s, timeout)          # banner + prompt
        s.sendall((cmd + "\r\n").encode())
        reply = recv_until_prompt(s, timeout)
        s.close()
        return reply
    except OSError:
        return None


def device_build(host: str, timeout: float = 5) -> "int | None":
    """Run `version` and return the running build number (or None)."""
    reply = run_command(host, "version", timeout)
    return parse_build(reply) if reply is not None else None


def expected_build() -> "int | None":
    """The build number just built (./.build_number written by VersionStamp), or None."""
    try:
        with open(".build_number") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def main() -> int:
    host   = os.environ["OTA_HOST"]
    url    = os.environ["OTA_URL"]
    expect = expected_build()
    before = device_build(host, timeout=10)
    if before is None:
        print(f"error: could not reach {host}:23 to read current version", file=sys.stderr)
        return 1
    print(f"==> device on build {before}"
          + (f"; pushing build {expect}" if expect else ""), flush=True)

    # Pre-flight: confirm the device actually HAS an `ota` command before the
    # (destructive) push — so a firmware that dropped it (MAX_COMMANDS overflow or
    # an image too old to support OTA) fails in ~1 s with a clear message instead
    # of after the full version-match wait. Bare `ota` prints "usage: ota <url>";
    # a missing command prints "unknown: ota".
    time.sleep(0.5)
    probe = run_command(host, "ota")
    if probe is not None:
        low = probe.lower()
        if "unknown" in low:
            print("error: device firmware has no `ota` command — it was dropped "
                  "(MAX_COMMANDS too small) or the running image is too old to "
                  "support OTA. Reflash via USB.", file=sys.stderr)
            return 1
        if "usage" not in low:
            print("warning: could not confirm the `ota` command exists; trying anyway.",
                  flush=True)
    time.sleep(0.5)   # let the single-client telnet server free the slot

    # Reach the prompt, fire the ota command, close. The connection drops once
    # sector-by-sector erase begins; the device downloads + reboots on its own.
    try:
        sock = socket.create_connection((host, 23), timeout=10)
    except OSError as e:
        print(f"error: could not connect to {host}:23 ({e})", file=sys.stderr)
        return 1
    recv_until_prompt(sock)
    sock.sendall(f"ota {url}\r\n".encode())
    print(f"==> sent: ota {url}", flush=True)
    print("==> erase+download interleaved — watch the HTTP server for a GET and "
          "USB serial for [ota] lines...", flush=True)
    try:
        sock.close()
    except OSError:
        pass

    # Poll for the device to return, then CONFIRM via the build number.
    for attempt in range(40):
        time.sleep(3)
        after = device_build(host)
        if after is None:
            if attempt % 5 == 4:
                print(f"  still waiting... ({(attempt + 1) * 3}s)", flush=True)
            continue

        target = expect if expect is not None else None
        if target is not None:
            if after == target:
                print(f"==> OTA confirmed: device is running build {after}.", flush=True)
                return 0
            print(f"==> OTA DID NOT TAKE: device is on build {after}, expected {target}. "
                  f"The download/flash failed — the device is still running the old "
                  f"image. Check USB serial for [ota] errors (no HTTP GET on the "
                  f"server = it never fetched the firmware).", file=sys.stderr)
            return 1
        # No .build_number to compare against — fall back to "did it change?".
        if after == before:
            print(f"==> OTA DID NOT TAKE: still on build {after} (unchanged). "
                  f"Check USB serial for [ota] errors.", file=sys.stderr)
            return 1
        print(f"==> OTA complete — device is running build {after} (was {before}).", flush=True)
        return 0

    print("==> timeout: device did not return. Check serial for details.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
