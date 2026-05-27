#!/usr/bin/env python3
"""Push OTA firmware to a running commander device via Telnet.

Environment variables (set by the calling shell script):
  OTA_HOST  — hostname or IP of the device  (e.g. pico.local)
  OTA_URL   — full HTTP URL of the fota image
               (e.g. http://192.168.1.x:8000/app_fota_image.bin)

Exits 0 on success, 1 on timeout or error.

Timeline: ~30 s erase + ~15 s download + ~20 s reboot/WiFi ≈ 65 s typical.
The HTTP server must stay alive for the entire erase+download window, so the
calling script is responsible for keeping it running and killing it on exit.
"""
import os, socket, time, sys


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


def try_connect(host: str, timeout: float = 5) -> str | None:
    """Return the banner string if the device answers on port 23, else None."""
    try:
        s = socket.create_connection((host, 23), timeout=timeout)
        s.settimeout(timeout)
        banner = strip_iac(s.recv(256)).decode("utf-8", errors="ignore").strip()
        s.close()
        return banner
    except OSError:
        return None


def main() -> int:
    host = os.environ["OTA_HOST"]
    url  = os.environ["OTA_URL"]

    # Connect with retries (device may still be booting or reconnecting to WiFi)
    sock = None
    for attempt in range(10):
        try:
            sock = socket.create_connection((host, 23), timeout=10)
            break
        except OSError as e:
            if attempt < 9:
                print(f"  waiting for {host}... ({e})", flush=True)
                time.sleep(3)
            else:
                print(f"error: could not connect to {host}:23", file=sys.stderr)
                return 1

    # Wait for the shell prompt
    sock.settimeout(15.0)
    raw = b""
    while True:
        try:
            chunk = sock.recv(256)
        except OSError:
            break
        if not chunk:
            break
        raw += chunk
        if strip_iac(raw).endswith(b"> "):
            break

    # Send the ota command then close immediately.
    # The telnet connection drops when sector-by-sector erase begins, but WiFi
    # stays alive — each sector is only ~100 ms IRQ-disabled.  The device
    # downloads firmware from the HTTP server and reboots.
    sock.sendall(f"ota {url}\r\n".encode())
    print(f"==> sent: ota {url}", flush=True)
    print("==> download+erase interleaved — HTTP hit should appear shortly...", flush=True)
    try:
        sock.close()
    except OSError:
        pass

    # Poll for device to reboot with new firmware
    for attempt in range(40):
        time.sleep(3)
        banner = try_connect(host)
        if banner:
            print(f"==> OTA complete — device is back: {banner}", flush=True)
            return 0
        if attempt % 5 == 4:
            print(f"  still waiting... ({(attempt + 1) * 3}s)", flush=True)

    print("==> timeout: device did not return. Check serial for details.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
