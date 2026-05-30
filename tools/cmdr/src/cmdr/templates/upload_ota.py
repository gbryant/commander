#!/usr/bin/env python3
"""
OTA upload for Arduino R4 WiFi — HTTP POST to the ArduinoOTA server (:65280).
The device must be in OTA mode first (the bum-ota script sends 'ota start' over
telnet, which closes telnet and starts the OTA listener).

Usage:
    python3 upload_ota.py <ip_or_host> [firmware.bin] [password]

Requires: pip install requests
"""

import sys
import hashlib
import base64
from os.path import exists, getsize

OTA_PORT = 65280


def calculate_md5(filename):
    h = hashlib.md5()
    with open(filename, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            h.update(chunk)
    return h.hexdigest()


def upload_firmware(ip_address, firmware_path, password=""):
    if not exists(firmware_path):
        print(f"ERROR: firmware not found: {firmware_path}")
        return False

    size = getsize(firmware_path)
    md5 = calculate_md5(firmware_path)
    print(f"OTA -> {ip_address}:{OTA_PORT}  ({size} bytes, md5 {md5})")

    import requests  # imported here so the message below is friendly

    url = f"http://{ip_address}:{OTA_PORT}/sketch"
    auth = base64.b64encode(f"arduino:{password}".encode()).decode()
    headers = {
        "Content-Type": "application/octet-stream",
        "Content-Length": str(size),
        "x-MD5": md5,
        "Authorization": f"Basic {auth}",
    }
    with open(firmware_path, "rb") as f:
        data = f.read()

    try:
        resp = requests.post(url, data=data, headers=headers, timeout=60)
    except ImportError:
        print("ERROR: the 'requests' package is required — pip install requests")
        return False
    except Exception as e:
        print(f"ERROR: upload failed: {e}")
        print("  - is the device in OTA mode? (send 'ota start' over telnet/serial)")
        print(f"  - is {ip_address} reachable on this network?")
        return False

    if resp.status_code == 200:
        print("SUCCESS — device will reboot into the new firmware.")
        return True
    print(f"ERROR: upload failed (HTTP {resp.status_code}): {resp.text}")
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ip = sys.argv[1]
    firmware = sys.argv[2] if len(sys.argv) >= 3 else ".pio/build/firmware.bin"
    password = sys.argv[3] if len(sys.argv) >= 4 else ""
    sys.exit(0 if upload_firmware(ip, firmware, password) else 1)


if __name__ == "__main__":
    main()
