"""cmdr — Commander framework project manager."""

import argparse
import configparser
import importlib.resources
import os
import re
import subprocess
import sys
from pathlib import Path

CONFIG_PATH = Path.home() / ".cmdr" / "config"


def load_config() -> configparser.ConfigParser:
    cfg = configparser.ConfigParser()
    if CONFIG_PATH.exists():
        cfg.read(CONFIG_PATH)
    return cfg


def save_config(cfg: configparser.ConfigParser) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        cfg.write(f)

REPO_URL = "https://github.com/gbryant/commander.git"

# The framework release a fresh project is pinned to. Scaffolds pin a TAG rather than floating on
# `main` so that a mistake pushed here can't reach into projects that were generated last month —
# they move when their owner says so (`cmdr pull` after `cmdr pin <ref>` / `--latest`, or `cmdr
# unpin` to track `main` deliberately).
#
# Versioning is two-part and the left digit means one thing: **this release breaks you**. Right
# digit for everything else. Bump this constant as part of cutting a release, so a project
# scaffolded today gets today's framework.
FRAMEWORK_TAG = "v1.1"

PICO_TARGETS = {
    "pico":  "pico_w",
    "pico2": "pico2_w",
}
ARDUINO_TARGETS = {"uno": "uno", "r4": "r4"}
STM32_TARGETS = {"bluepill": "bluepill_f103c8"}   # PlatformIO + native CMSIS (not Arduino)
ZEPHYR_TARGETS = {"unoq": "arduino_uno_q"}        # Zephyr/west; dual-brain (MCU + paired Linux)
TARGETS = {**PICO_TARGETS, "esp32": "esp32", **ARDUINO_TARGETS, **STM32_TARGETS, **ZEPHYR_TARGETS}

VALID_FLASH_MB  = {2, 4, 8, 16, 32}
VALID_PSRAM_MB  = {0, 2, 4, 8}
USB_JTAG_CHIPS  = {"esp32s3", "esp32s2", "esp32c3", "esp32c6", "esp32h2"}
PSRAM_OCT_CHIPS = {"esp32s3"}

# ── Pico templates (placeholders: __NAME__, __BOARD__) ────────────────────────

PICO_CMAKE_TEMPLATE = """\
cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD __BOARD__ CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/FreeRTOS_Kernel_import.cmake)

project(__NAME__ C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

include(FetchContent)
FetchContent_Declare(commander
    GIT_REPOSITORY """ + REPO_URL + """
    GIT_TAG        """ + FRAMEWORK_TAG + """
)
# Local commander source override — managed by `cmdr link` / `cmdr unlink`.
include(${CMAKE_SOURCE_DIR}/commander_local.cmake OPTIONAL)
FetchContent_MakeAvailable(commander)

add_executable(__NAME__ main.cpp)
commander_generate_scripts(__NAME__)
target_link_libraries(__NAME__ PRIVATE commander::pico_runner)
pico_enable_stdio_usb(__NAME__ 1)
pico_enable_stdio_uart(__NAME__ 0)
pico_add_extra_outputs(__NAME__)
"""

# ── Shared C++ template (placeholders: __NAME__) ──────────────────────────────

MAIN_CPP_TEMPLATE = """\
#include "commander.h"
#include "core/SystemModule.h"
#include "secrets.h"

static SystemModule sysModule;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.wifi_ssid     = WIFI_SSID;
    cfg.wifi_password = WIFI_PASSWORD;
    cfg.hostname      = "__NAME__";
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "__NAME__";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry& reg) {
    reg.registerModule(sysModule);
}
"""

SECRETS_H_TEMPLATE = """\
#pragma once
#define WIFI_SSID     "__SSID__"
#define WIFI_PASSWORD "__PASSWORD__"
"""

# ── ESP32 templates (placeholders: __NAME__, __CHIP__) ───────────────────────

ESP32_CMAKE_TEMPLATE = """\
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH not set — run 'esp' to load the ESP-IDF environment")
endif()

# Download commander source before IDF initializes — download ONLY, we must not process
# commander's own (Pico) CMakeLists.txt. SOURCE_SUBDIR points at a dir with no CMakeLists.txt
# (include/), so MakeAvailable populates the source but skips add_subdirectory. (This replaces
# the deprecated FetchContent_Populate(commander) — same effect, no CMP0169 warning.)
include(FetchContent)
FetchContent_Declare(commander
    GIT_REPOSITORY """ + REPO_URL + """
    GIT_TAG        """ + FRAMEWORK_TAG + """
    SOURCE_SUBDIR  include
)
# Local commander source override — managed by `cmdr link` / `cmdr unlink`.
include(${CMAKE_SOURCE_DIR}/commander_local.cmake OPTIONAL)
FetchContent_MakeAvailable(commander)

set(COMMANDER_ROOT ${commander_SOURCE_DIR})
list(APPEND EXTRA_COMPONENT_DIRS ${commander_SOURCE_DIR}/runners/esp32)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(__NAME__)

# Stamp the project name + build number into the firmware so `version` reports the
# running build (and bum-ota can confirm an OTA took). Provided by the commander
# runner's project_include.cmake; must be called after project().
commander_stamp_version()
"""

ESP32_MAIN_CMAKE_TEMPLATE = """\
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ".."
    REQUIRES commander_runner
)
"""

ESP32_BUM_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$DIR/build"
"$DIR/upload"
"$DIR/monitor"
"""

# Source the ESP-IDF env so build/upload are self-contained — you don't have to run
# `esp` / `. $IDF_PATH/export.sh` first. No-op if idf.py is already on PATH. Tries
# $IDF_EXPORT, the path captured at `cmdr init`, $IDF_PATH/export.sh, the setup-sdks.sh
# install dir (~/u-developer/esp-idf), then the standard install locations. Override
# IDF_EXPORT to point at your own export.sh. (__IDF_EXPORT__
# is filled in at init from the environment's $IDF_PATH, blank if IDF wasn't active.)
ESP32_ENV_PREAMBLE = """\
if ! command -v idf.py >/dev/null 2>&1; then
    for _exp in "${IDF_EXPORT:-}" "__IDF_EXPORT__" "${IDF_PATH:-}/export.sh" \\
                "$HOME/u-developer/esp-idf/export.sh" \\
                "$HOME/esp/esp-idf/export.sh" "$HOME/esp-idf/export.sh"; do
        [ -n "$_exp" ] && [ -f "$_exp" ] && { . "$_exp" >/dev/null 2>&1; break; }
    done
fi
command -v idf.py >/dev/null 2>&1 || {
    echo "esp-idf not found — run 'esp' (or set IDF_EXPORT to your esp-idf/export.sh)" >&2; exit 1; }
"""

ESP32_BUILD_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
""" + ESP32_ENV_PREAMBLE + """\
BUILD="$DIR/build-esp32"
if [ ! -f "$BUILD/config/sdkconfig.json" ]; then
    idf.py -B "$BUILD" set-target __CHIP__
fi
idf.py -B "$BUILD" build
"""

ESP32_UPLOAD_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
""" + ESP32_ENV_PREAMBLE + """\
PORT=$(python3 "$DIR/scripts/find_port.py" __CHIP__)
echo "Flashing $PORT..."
idf.py -B "$DIR/build-esp32" -p "$PORT" flash
"""

ESP32_BUM_OTA_SCRIPT = """\
#!/bin/bash
# Build __NAME__ and push firmware via OTA.
# Usage: ./bum-ota [host]   default: __NAME__.local
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${1:-__NAME__.local}"
OTA_PORT=8000

echo "==> Building..."
"$DIR/build"

BIN="$DIR/build-esp32/__NAME__.bin"
[[ -f "$BIN" ]] || { echo "Binary not found: $BIN"; exit 1; }

IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)
[[ -n "$IP" ]] || { echo "Could not determine local IP — check en0/en1."; exit 1; }
URL="http://$IP:$OTA_PORT/__NAME__.bin"

python3 -m http.server "$OTA_PORT" --directory "$DIR/build-esp32" &
SERVER_PID=$!
trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT
sleep 1

echo "==> Serving $URL"
echo "==> Connecting to $HOST..."
OTA_HOST="$HOST" OTA_URL="$URL" python3 "$DIR/scripts/ota_push.py"
echo "==> Done."

# Wait for the device to reboot onto the new firmware, then open a session.
echo "==> Waiting for $HOST to come back up..."
for _ in $(seq 1 60); do
    python3 -c "import socket,sys; s=socket.socket(); s.settimeout(1.5); s.connect((sys.argv[1],23)); s.close()" "$HOST" 2>/dev/null && break
    sleep 1
done
echo "==> Opening telnet ($HOST) — Ctrl-] then 'quit' to exit"
telnet "$HOST"
"""

# ── Arduino templates (placeholders: __NAME__, __BOARD_ID__) ─────────────────

ARDUINO_UNO_PIO_TEMPLATE = """\
[platformio]
src_dir = src

[env:__NAME__]
platform = atmelavr
board = uno
framework = arduino
monitor_speed = 115200
extra_scripts =
    pre:scripts/patch_freertos.py
    pre:scripts/version_stamp.py
build_flags =
    -DCOMMANDER_UNO_RUNNER
lib_deps =
    """ + REPO_URL + """
    https://github.com/feilipu/Arduino_FreeRTOS_Library.git
"""

ARDUINO_R4_PIO_TEMPLATE = """\
[platformio]
src_dir = src

[env:__NAME__]
platform = renesas-ra
board = uno_r4_wifi
framework = arduino
monitor_speed = 115200
extra_scripts =
    pre:scripts/version_stamp.py
build_flags =
    -DCOMMANDER_R4_RUNNER
    -I${PROJECT_DIR}
    ; FreeRTOS hardening: report stack overflow / heap exhaustion instead of a
    ; silent corruption that wedges the ESP32-S3 bridge. Hooks live in the runner.
    -DconfigCHECK_FOR_STACK_OVERFLOW=2
    -DconfigUSE_MALLOC_FAILED_HOOK=1
lib_deps =
    """ + REPO_URL + """
"""

ARDUINO_MAIN_CPP_TEMPLATE = """\
#include "commander.h"
#include "core/SystemModule.h"

static SystemModule sysModule;

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "__NAME__";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry& reg) {
    reg.registerModule(sysModule);
}
"""

# Hook-based main (R4 / Pico / ESP32): modules are managed by `cmdr module`
# via the generated commander_modules.h, so commander_setup just calls the hook.
HOOK_MAIN_CPP_TEMPLATE = """\
#include "commander.h"
#include "commander_modules.h"   // generated by cmdr — run `cmdr module`
#include "secrets.h"

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.wifi_ssid     = WIFI_SSID;
    cfg.wifi_password = WIFI_PASSWORD;
    cfg.hostname      = "__NAME__";
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "__NAME__";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry& reg) {
    commander_register_modules(reg);
}
"""

# Uno hook main: same module-system hook as HOOK_MAIN but no WiFi/secrets.
UNO_MAIN_CPP_TEMPLATE = """\
#include "commander.h"
#include "commander_modules.h"   // generated by cmdr — run `cmdr module`

extern "C" CommanderConfig commander_config() {
    CommanderConfig cfg;
    cfg.uart_baud     = 115200;
    cfg.uart_greeting = "__NAME__";
    return cfg;
}

extern "C" void commander_setup(CommandRegistry& reg) {
    commander_register_modules(reg);
}
"""

ARDUINO_BUM_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$DIR/build"
"$DIR/upload"
"$DIR/monitor"
"""

ARDUINO_BUILD_SCRIPT = """\
#!/bin/bash
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
pio run -e __NAME__
"""

ARDUINO_UPLOAD_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=$(python3 "$DIR/scripts/find_port.py" __BOARD_ID__)
pio run -e __NAME__ -t upload --upload-port "$PORT"
"""

ARDUINO_MONITOR_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=""
for i in $(seq 1 20); do
    # exit 1 = not enumerated yet (keep waiting, quietly); anything else is a
    # real problem — e.g. 2 = several boards match, which polling can't fix.
    PORT=$(python3 "$DIR/scripts/find_port.py" __BOARD_ID__ 2>/dev/null) && break
    [ $? -ne 1 ] && { python3 "$DIR/scripts/find_port.py" __BOARD_ID__ >/dev/null; exit 1; }
    [ $i -eq 1 ] && echo "Waiting for __BOARD_ID__..."
    sleep 0.5
done
[ -n "$PORT" ] || { echo "error: __BOARD_ID__ port did not appear" >&2; exit 1; }
echo "Connecting to $PORT  (Ctrl-T q to quit)"
tio --baudrate 115200 "$PORT"
"""

ARDUINO_R4_BUM_OTA_SCRIPT = """\
#!/bin/bash
# Build __NAME__, then push via OTA. Builds FIRST so the device's telnet/mDNS are
# torn down only once a fresh binary exists; then arms the device (sends 'ota
# start' over telnet — the device closes telnet and starts the OTA listener on
# :65280 so the two never contend for the WiFiS3 socket pool) and HTTP-POSTs the
# firmware. Requires: pip install requests
# Usage: ./bum-ota [host]   default: __NAME__.local
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${1:-__NAME__.local}"
BIN="$DIR/.pio/build/__NAME__/firmware.bin"

# Build first — don't arm OTA (which tears down the device's telnet + mDNS and
# starts a timed listener) until we actually have a fresh binary.
"$DIR/build"
[ -f "$BIN" ] || { echo "Build produced no binary: $BIN"; exit 1; }

echo "Resolving $HOST..."
# Fail with a clear message instead of a Python traceback if the name doesn't
# resolve (almost always: the device is still booting / not yet on WiFi / mDNS
# not up — give it a few seconds after a fresh flash, then retry).
IP=$(python3 -c "import socket,sys; print(socket.gethostbyname(sys.argv[1]))" "$HOST" 2>/dev/null) \
    || { echo "Could not resolve $HOST — is it powered and on WiFi? (try: ping $HOST)"; exit 1; }
echo "  $HOST -> $IP"

echo "Arming OTA via telnet ($IP:23) — waiting for connection to drop..."
python3 - "$IP" <<'PYEOF'
import sys, socket, time
ip = sys.argv[1]
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((ip, 23)); s.settimeout(20)
time.sleep(0.5)
try: print(s.recv(4096).decode("utf-8", "replace"), end="", flush=True)
except OSError: pass
s.sendall(b"ota start\\r\\n")
try:
    while True:
        d = s.recv(1024)
        if not d: break
        print(d.decode("utf-8", "replace"), end="", flush=True)
except OSError: pass
s.close()
PYEOF
echo "OTA mode active."

python3 "$DIR/scripts/upload_ota.py" "$IP" "$BIN"

# Wait for the device to reboot onto the new firmware, then open a session.
echo "Waiting for $HOST to come back up..."
for _ in $(seq 1 60); do
    python3 -c "import socket,sys; s=socket.socket(); s.settimeout(1.5); s.connect((sys.argv[1],23)); s.close()" "$HOST" 2>/dev/null && break
    sleep 1
done
echo "Opening telnet ($HOST) — Ctrl-] then 'quit' to exit"
telnet "$HOST"
"""

ESP32_MONITOR_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=""
for i in $(seq 1 20); do
    # exit 1 = not enumerated yet (keep waiting, quietly); anything else is a
    # real problem — e.g. 2 = several boards match, which polling can't fix.
    PORT=$(python3 "$DIR/scripts/find_port.py" __CHIP__ 2>/dev/null) && break
    [ $? -ne 1 ] && { python3 "$DIR/scripts/find_port.py" __CHIP__ >/dev/null; exit 1; }
    [ $i -eq 1 ] && echo "Waiting for __CHIP__..."
    sleep 0.5
done
[ -n "$PORT" ] || { echo "error: __CHIP__ port did not appear" >&2; exit 1; }
echo "Connecting to $PORT  (Ctrl-T q to quit)"
tio --baudrate 115200 "$PORT"
"""

# ── STM32 Bluepill templates (placeholders: __NAME__) ────────────────────────
# Native CMSIS + FreeRTOS + raw TinyUSB. Commander source, the FreeRTOS kernel, and
# TinyUSB are assembled by scripts/stm32_build.py (shipped as a template), so the
# platformio.ini just needs lib_deps + flags. Console is USB CDC; flashing via ST-Link.

BLUEPILL_PIO_TEMPLATE = """\
[platformio]
src_dir = src

[env:__NAME__]
platform = ststm32
board = bluepill_f103c8
framework = cmsis
upload_protocol = stlink
debug_tool = stlink
monitor_speed = 115200
extra_scripts =
    pre:scripts/stm32_build.py
build_flags =
    -DCOMMANDER_BLUEPILL_RUNNER
    -DCOMMANDER_STM32_USB_CONSOLE
    -DSTM32F103xB
lib_deps =
    """ + REPO_URL + """
"""

BLUEPILL_BUILD_SCRIPT = """\
#!/bin/bash
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
pio run -e __NAME__
"""

BLUEPILL_UPLOAD_SCRIPT = """\
#!/bin/bash
# Flash via ST-Link (SWD). openocd auto-detects the ST-Link — no port needed.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
pio run -e __NAME__ -t upload
"""

BLUEPILL_MONITOR_SCRIPT = """\
#!/bin/bash
# USB-CDC console at 115200 (auto-detected by VID:PID 0483:5740). With a weak D+
# pull-up you may need to press the board's reset button after plugging in.
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=""
for i in $(seq 1 20); do
    # exit 1 = not enumerated yet (keep waiting, quietly); anything else is a
    # real problem — e.g. 2 = several boards match, which polling can't fix.
    PORT=$(python3 "$DIR/scripts/find_port.py" bluepill 2>/dev/null) && break
    [ $? -ne 1 ] && { python3 "$DIR/scripts/find_port.py" bluepill >/dev/null; exit 1; }
    [ $i -eq 1 ] && echo "Waiting for the bluepill USB-CDC port (press reset after plugging in)..."
    sleep 0.5
done
[ -n "$PORT" ] || { echo "error: bluepill USB-CDC port did not appear" >&2; exit 1; }
echo "Connecting to $PORT  (Ctrl-T q to quit)"
tio --baudrate 115200 "$PORT"
"""

# `bum` when DFU is enabled: build + flash over USB (no ST-Link) + monitor.
BLUEPILL_DFU_BUM_SCRIPT = """\
#!/bin/bash
# DFU dev loop: build + flash over USB (no ST-Link) + monitor. upload-bluepill-usb
# auto-reboots the running app into DFU; if the board isn't enumerating, type
# `bootloader` in the shell (or press reset) and re-run.
# For an ST-Link upload instead: ./flash-bluepill-bootloader (once) then ./upload.
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$DIR/upload-bluepill-usb"
"$DIR/monitor"
"""

PFB_BLOCK = """\
if(DEFINED ENV{PFB_PATH})
    set(PFB_PATH "$ENV{PFB_PATH}")
else()
    set(PFB_PATH "$ENV{HOME}/u-developer/pico_fota_bootloader")
endif()
set(PFB_WITH_IMAGE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(PFB_WITH_SHA256_HASHING   ON  CACHE BOOL "" FORCE)
add_subdirectory(${PFB_PATH} pfb_build)\
"""

# ── partition table generation ────────────────────────────────────────────────
# The table is composed from the enabled features (OTA, filesystem) so they stack
# instead of clobbering each other. `cmdr enable ota` / `enable littlefs` each
# re-derive the *other* feature's state from the existing partitions.csv, so the
# order they're enabled in doesn't matter.

_ALIGN = 0x10000          # app partitions must be 64 KB aligned; we align everything to it
_APP_START = 0x10000      # first app/factory partition offset (after nvs + otadata)


def _detect_flash_mb(default: int = 16) -> int:
    sdk = Path("sdkconfig.defaults")
    if sdk.exists():
        m = re.search(r"CONFIG_ESPTOOLPY_FLASHSIZE_(\d+)MB=y", sdk.read_text())
        if m:
            return int(m.group(1))
    return default


def _default_app_budget(flash_mb: int) -> int:
    # App region size when a filesystem takes "the rest" — a balanced split that
    # leaves the remaining flash for the FS. Used only when an FS has no fixed size.
    return {4: 0x300000, 8: 0x400000}.get(flash_mb, flash_mb * 0x100000 // 2)


def parse_partitions(text: str) -> "tuple[bool, list]":
    """Recover (ota_enabled, [(label, subtype, size|None), …]) from a partitions.csv."""
    ota = False
    fs: "list" = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 3:
            continue
        name, ptype, subtype = cols[0], cols[1], cols[2]
        if ptype == "app" and subtype.startswith("ota_"):
            ota = True
        elif ptype == "data" and subtype in ("littlefs", "spiffs", "fat"):
            size = None
            if len(cols) >= 5 and cols[4]:
                try:
                    size = int(cols[4], 0)
                except ValueError:
                    size = None
            fs.append((name, subtype, size))
    return ota, fs


def compose_partitions(flash_mb: int, ota: bool, fs: "list") -> str:
    """Build a partitions.csv for the given flash size, OTA flag, and FS partitions.

    fs is a list of (label, subtype, size|None); None means "take the remaining
    flash" (split evenly if several). App slots reflow to make room for fixed-size
    filesystems, so adding an FS to an OTA project (or vice versa) just works.
    """
    flash = flash_mb * 0x100000
    usable = flash - _APP_START
    fixed_fs = sum(sz for (_, _, sz) in fs if sz)
    has_rest = any(sz is None for (_, _, sz) in fs)
    app_budget = (_default_app_budget(flash_mb) if has_rest else usable - fixed_fs) & ~(_ALIGN - 1)
    if app_budget < _ALIGN:
        die(f"no room for the app on {flash_mb} MB flash — filesystem too large")

    rows = []
    if ota:
        rows += [("nvs", "data", "nvs", 0x9000, 0x5000),
                 ("otadata", "data", "ota", 0xe000, 0x2000)]
    else:
        rows += [("nvs", "data", "nvs", 0x9000, 0x7000)]   # fills 0x9000..0x10000
    cur = _APP_START
    if ota:
        slot = (app_budget // 2) & ~(_ALIGN - 1)
        rows += [("app0", "app", "ota_0", cur, slot)]; cur += slot
        rows += [("app1", "app", "ota_1", cur, slot)]; cur += slot
    else:
        rows += [("factory", "app", "factory", cur, app_budget)]; cur += app_budget

    rest_pool = flash - cur - fixed_fs
    n_rest = sum(1 for (_, _, sz) in fs if sz is None)
    for (label, subtype, sz) in fs:
        size = (sz if sz else rest_pool // max(n_rest, 1)) & ~(_ALIGN - 1)
        if size < _ALIGN:
            die(f"no room for filesystem '{label}' on {flash_mb} MB flash")
        rows += [(label, "data", subtype, cur, size)]; cur += size

    if cur > flash:
        die(f"partition layout {hex(cur)} exceeds {flash_mb} MB flash")

    fsnote = "".join(f" + {l}({st})" for (l, t, st, _, _) in rows
                     if t == "data" and st in ("littlefs", "spiffs", "fat"))
    out = [f"# {flash_mb} MB flash — {'dual OTA' if ota else 'single app'}{fsnote}",
           "# Name,     Type, SubType,  Offset,     Size"]
    for (name, ptype, subtype, off, size) in rows:
        out.append(f"{name + ',':<11} {ptype + ',':<5} {subtype + ',':<9} "
                   f"{hex(off) + ',':<11} {hex(size)}")
    return "\n".join(out) + "\n"


# ── sdkconfig generation ──────────────────────────────────────────────────────

def make_sdkconfig(chip: str, flash_mb: int, psram_mb: int) -> str:
    lines = [f"# {chip} — {flash_mb} MB flash"
             + (f", {psram_mb} MB PSRAM" if psram_mb else "")]
    lines.append(f"CONFIG_ESPTOOLPY_FLASHSIZE_{flash_mb}MB=y")
    lines.append(f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash_mb}MB"')
    if psram_mb:
        mode = "OCT" if chip in PSRAM_OCT_CHIPS else "QUAD"
        lines += [
            "CONFIG_SPIRAM=y",
            f"CONFIG_SPIRAM_MODE_{mode}=y",
            "CONFIG_SPIRAM_SPEED_80M=y",
            "CONFIG_SPIRAM_TYPE_AUTO=y",
        ]
    if chip in USB_JTAG_CHIPS:
        lines.append("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y")
    return "\n".join(lines) + "\n"


# ── Helpers ───────────────────────────────────────────────────────────────────

# ── Arduino Uno Q templates (Zephyr/west; dual-brain MCU + paired Linux) ──────
# The Uno Q is its own board type, not a generic Zephyr target (that generalization is
# deferred until a second such board). cmdr generates software only; the board-mutating
# steps (boot mode, masking the router, installing the broker service) are SCRIPTS the
# user runs explicitly — each with a revert. See the scaffolded README.

UNOQ_CMAKE_TEMPLATE = """\
cmake_minimum_required(VERSION 3.20.0)

# Pull commander before Zephyr initializes — download ONLY (don't process commander's own
# Pico CMakeLists). SOURCE_SUBDIR=include has no CMakeLists.txt, so MakeAvailable populates
# the source but skips add_subdirectory (the non-deprecated way to do FetchContent_Populate).
# GIT_TAG is a release tag; `cmdr pin <ref>` / `cmdr unpin` move it.
include(FetchContent)
FetchContent_Declare(commander GIT_REPOSITORY """ + REPO_URL + """ GIT_TAG """ + FRAMEWORK_TAG + """ SOURCE_SUBDIR include)
FetchContent_MakeAvailable(commander)
set(COMMANDER ${commander_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(__NAME__)

# The Uno Q's two brains talk over the multiplexed channel bus (ch0 console + pub/sub),
# not a plain UART console. The runner switches transport on this flag.
add_compile_definitions(COMMANDER_ENABLE_CHANNELS)

target_include_directories(app PRIVATE ${COMMANDER} ${COMMANDER}/include)
target_sources(app PRIVATE
    src/main.cpp
    ${COMMANDER}/runners/zephyr/runner.cpp
    ${COMMANDER}/core/CommandRegistry.cpp
    ${COMMANDER}/transport/channels/ChannelBusRunner.cpp
    ${COMMANDER}/platform/zephyr/ZephyrIRModule.cpp   # dead-stripped unless `ir` is enabled
    ${COMMANDER}/hal/zephyr/hal.cpp
)
"""

UNOQ_PRJCONF_TEMPLATE = """\
# commander on Zephyr — Uno Q (STM32U585 / M33)
CONFIG_CPP=y
CONFIG_STD_CPP17=y
CONFIG_CPP_EXCEPTIONS=n

CONFIG_SERIAL=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
CONFIG_UART_INTERRUPT_DRIVEN=y     # RX must be ISR-driven (poll overruns at 115200)
CONFIG_GPIO=y                      # IR edge capture

CONFIG_NEWLIB_LIBC=y               # commander uses snprintf etc.
CONFIG_MAIN_STACK_SIZE=2048
"""

UNOQ_OVERLAY_TEMPLATE = """\
/* Route commander's console to lpuart1 = the QRB bridge UART (-> /dev/ttyHS1).
 * The board default (usart1) is the Arduino header Serial1 (D0/D1), not the Linux side. */
/ {
    chosen {
        zephyr,console = &lpuart1;
        zephyr,shell-uart = &lpuart1;
    };

    /* IR receiver OUT pin — used only when the `ir` module is enabled. Default D5
     * (arduino_header index 11 = &gpioa 11). Edit to move the pin. */
    zephyr,user {
        ir-gpios = <&arduino_header 11 GPIO_ACTIVE_HIGH>;
    };
};
"""

# Dev loop runs on the Mac; build is west, flash is openocd-over-adb (west flash for this
# board isn't working upstream). Toolchain env is overridable — defaults to the Arm GNU
# Toolchain because the Zephyr SDK has no Intel-Mac build.
UNOQ_ENV_PREAMBLE = """\
# Zephyr build env (override any of these for your install). The default workspace is
# ~/u-developer/zephyrproject — what commander's setup-sdks.sh --zephyr creates — with
# a fallback to ~/zephyrproject for a hand-made checkout.
_ZP="${ZEPHYRPROJECT:-$HOME/u-developer/zephyrproject}"
[ -d "$_ZP" ] || _ZP="$HOME/zephyrproject"
source "${ZEPHYR_VENV:-$_ZP/.venv}/bin/activate" 2>/dev/null || true
export ZEPHYR_BASE="${ZEPHYR_BASE:-$_ZP/zephyr}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"
export GNUARMEMB_TOOLCHAIN_PATH="${GNUARMEMB_TOOLCHAIN_PATH:-/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi}"
GDB="${GDB:-$GNUARMEMB_TOOLCHAIN_PATH/bin/arm-none-eabi-gdb}"
"""

UNOQ_BUILD_SCRIPT = """\
#!/bin/bash
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
""" + UNOQ_ENV_PREAMBLE + """\
# -d build-unoq: west's default build dir is ./build, which collides with this `build` script.
west build -b arduino_uno_q -d build-unoq "$@"
"""

UNOQ_FLASH_SCRIPT = """\
#!/bin/bash
# Flash the M33 over the QRB's on-board SWD (openocd via adb). One arduino-debug at a time.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
""" + UNOQ_ENV_PREAMBLE + """\
adb shell "pkill -f openocd" 2>/dev/null || true
adb forward tcp:3333 tcp:3333 >/dev/null
( adb shell arduino-debug >/tmp/cmdr-unoq-debug.log 2>&1 & )
sleep 6
"$GDB" build-unoq/zephyr/zephyr.elf -batch \\
  -ex "target extended-remote localhost:3333" \\
  -ex "monitor reset halt" -ex load -ex "monitor reset run" -ex detach -ex quit
adb shell "pkill -f openocd" 2>/dev/null || true
adb forward --remove tcp:3333 2>/dev/null || true
echo "flashed. (if the M33 boots the ROM bootloader instead of the app, run ./enable-flash-boot once)"
"""

UNOQ_MONITOR_SCRIPT = """\
#!/bin/bash
# The Mac's console is ch0 of the channel bus, carried over the USB-CDC gadget by the
# broker service. Needs ./install-broker to have been run on the board.
set -e
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "no /dev/cu.usbmodem* found — is the board plugged in?"; exit 1; }
echo "Connecting to $PORT  (Ctrl-T q to quit). Type 'help'."
tio --baudrate 115200 "$PORT"
"""

UNOQ_BUM_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$DIR/build"
"$DIR/flash"
"$DIR/monitor"
"""

# ── Board-mutating scripts (the user runs these; each is reversible) ──────────

UNOQ_ENABLE_FLASH_BOOT_SCRIPT = """\
#!/bin/bash
# ONE-TIME, board-modifying: set STM32U585 option bytes so the M33 boots from FLASH
# (nSWBOOT0=0, nBOOT0=1) instead of its ROM bootloader. Without this, commander never
# runs. Recoverable: SWD works in any boot mode; revert with ./restore-arduino. Never
# touches the RDP byte. See docs/zephyr-hal-spike.md.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
""" + UNOQ_ENV_PREAMBLE + """\
adb shell "pkill -f openocd" 2>/dev/null || true
adb forward tcp:3333 tcp:3333 >/dev/null
( adb shell arduino-debug >/tmp/cmdr-unoq-debug.log 2>&1 & )
sleep 6
CUR=$("$GDB" -batch -ex "target extended-remote localhost:3333" -ex "monitor halt" \\
  -ex 'monitor stm32l4x option_read 0 0x40' 2>/dev/null | grep -oiE '0x[0-9a-f]{8}' | tail -1)
echo "current FLASH_OPTR = ${CUR:-?}"
if [ "$CUR" = "0x1beff8aa" ]; then
  echo "already set to boot from flash — nothing to do."
else
  read -p "Write option bytes to boot from flash (0x1beff8aa)? [y/N] " ok
  if [ "$ok" = "y" ] || [ "$ok" = "Y" ]; then
    "$GDB" -batch -ex "target extended-remote localhost:3333" -ex "monitor halt" \\
      -ex "monitor stm32l4x unlock 0" \\
      -ex "monitor stm32l4x option_write 0 0x40 0x1beff8aa 0x0c000000" \\
      -ex "monitor stm32l4x option_load 0" -ex quit 2>&1 | grep -iE "written|option" || true
    sleep 1
    VTOR=$("$GDB" -batch -ex "target extended-remote localhost:3333" -ex "monitor reset run" \\
      -ex "shell sleep 1" -ex "monitor halt" \\
      -ex 'printf "0x%08x\\n", *(unsigned int*)0xE000ED08' 2>/dev/null | grep -oiE '0x08000000|0x0bf90000' | tail -1)
    echo "VTOR=$VTOR  ($([ "$VTOR" = "0x08000000" ] && echo 'boots commander from flash ✓' || echo 'still bootloader — retry'))"
  fi
fi
adb shell "pkill -f openocd" 2>/dev/null || true
adb forward --remove tcp:3333 2>/dev/null || true
"""

UNOQ_INSTALL_BROKER_SCRIPT = """\
#!/bin/bash
# Thin shim: the real install-broker logic lives in the commander framework
# (dev/unoq/install_broker.sh), fetched into build-unoq/ by ./build. This stub just locates
# and runs it — so the logic updates with the framework (cmdr pull / clean + build) and never
# goes stale in the project. (Board-modifying + reversible with ./restore-arduino; needs the
# board sudo password. The impl pushes the broker from the fetched source — no GitHub auth on
# the board, works for a private repo.)
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"

IMPL=$(find build-unoq -path '*/dev/unoq/install_broker.sh' 2>/dev/null | head -1)
if [ -z "$IMPL" ]; then
  echo "commander framework not fetched yet — run ./build first (it downloads commander via"
  echo "FetchContent), then re-run ./install-broker."
  exit 1
fi
exec bash "$IMPL" "$@"
"""

UNOQ_RESTORE_ARDUINO_SCRIPT = """\
#!/bin/bash
# Revert the board to the stock Arduino App Lab flow: stop/disable commander's services (broker
# + bridge), restore AND start the Arduino router stack, and (optionally) put the M33 boot bytes
# back so the standard DFU sketch-flash works. The inverse of install-broker + enable-flash-boot.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
# $BOARD_SUDO_PW skips the prompt, so this runs from a script or an agent session with no TTY.
# Without it and without a terminal, `read` hits EOF and `set -e` aborts with NO output at all.
if [ -n "$BOARD_SUDO_PW" ]; then
  PW="$BOARD_SUDO_PW"
elif [ -t 0 ]; then
  read -s -p "Board sudo password: " PW; echo
else
  echo "no terminal to prompt for the board sudo password." >&2
  echo "run this from a terminal, or pass it in:  BOARD_SUDO_PW=... ./restore-arduino" >&2
  exit 1
fi
run() { adb shell "echo '$PW' | sudo -S bash -c '$1'"; }

echo "==> stopping commander's services, restoring + starting the Arduino router stack"
# Both commander services own /dev/ttyHS1 (the broker directly, the bridge via socat), and so
# does the router — only one can. Stop BOTH before handing the link back, or the loser
# crash-loops on "Device or resource busy" every 2 s, silently, forever (Restart=always).
run 'systemctl disable --now commander-broker.service 2>/dev/null; systemctl disable --now commander-bridge.service 2>/dev/null; true'
# Restore the unit files, then actually START the router: unmasking alone leaves the stock flow
# dead until the next reboot, which is the same "masking is not stopping" trap in reverse.
run 'cd /etc/systemd/system && for u in arduino-router.service arduino-router-serial.service arduino-router-serial.path; do [ -L $u ] && rm -f $u; [ -f $u.commander-bak ] && mv $u.commander-bak $u; done; systemctl daemon-reload; systemctl start arduino-router.service arduino-router-serial.path 2>/dev/null || true; systemctl start arduino-router-serial.service 2>/dev/null || true'

sleep 2
if adb shell "pgrep -x arduino-router >/dev/null && echo yes" 2>/dev/null | grep -q yes; then
  echo "arduino-router running - App Lab works without a reboot"
else
  echo "arduino-router did not start; reboot the board (adb reboot) and it will come back" >&2
fi

# Optional + destructive-ish, so default to NO without a terminal rather than letting `read`
# fail the script after the router restore already succeeded.
if [ -t 0 ]; then
  read -p "Also revert the M33 boot bytes to factory (BOOT0-pin)? [y/N] " ok
else
  ok=n
  echo "(no terminal: leaving the M33 boot bytes alone; run from a terminal to revert them)"
fi
if [ "$ok" = "y" ] || [ "$ok" = "Y" ]; then
""" + UNOQ_ENV_PREAMBLE + """\
  adb shell "pkill -f openocd" 2>/dev/null || true
  adb forward tcp:3333 tcp:3333 >/dev/null; ( adb shell arduino-debug >/tmp/cmdr-unoq-debug.log 2>&1 & ); sleep 6
  "$GDB" -batch -ex "target extended-remote localhost:3333" -ex "monitor halt" \\
    -ex "monitor stm32l4x unlock 0" \\
    -ex "monitor stm32l4x option_write 0 0x40 0x1feff8aa 0x0c000000" \\
    -ex "monitor stm32l4x option_load 0" -ex quit 2>&1 | grep -iE "written|option" || true
  adb shell "pkill -f openocd" 2>/dev/null || true; adb forward --remove tcp:3333 2>/dev/null || true
fi
echo "restored."
"""

UNOQ_DEPLOY_SBC_SCRIPT = """\
#!/bin/bash
# Deploy this project's SBC-side tools to the Uno Q's Debian home (/home/arduino). These are the
# scripts that run NEXT TO the broker and read its channel sockets — e.g. the channel IR tools
# that `cmdr module enable ir` drops into bin/. Re-runnable; run it again after enabling a module
# that ships SBC tools, or after editing one.
#
#   ./deploy-sbc                                  push the tools (+ seed maps/)
#   ./deploy-sbc --service "ir_speak.py --greeting"   ...and run that one at every boot
#   ./deploy-sbc --stop-service ir_speak.py       stop + disable it again
#
# --service turns the board into an appliance: power it up with no computer attached and the
# tool is already running. It installs a systemd --user unit (user, not system: these tools need
# the session bus to reach PipeWire audio, and linger keeps user units running with nobody
# logged in). Pair it with unoq-tools `bt.py autoconnect on <MAC>` so the speaker reconnects by
# itself, or a headless board comes up mute.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
DEST=/home/arduino

SERVICE=""; STOP_SERVICE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --service)      SERVICE="$2"; shift 2 ;;
    --stop-service) STOP_SERVICE="$2"; shift 2 ;;
    *) echo "usage: ./deploy-sbc [--service \\"<tool.py> [args]\\"] [--stop-service <tool.py>]" >&2; exit 2 ;;
  esac
done

usr() { adb shell "XDG_RUNTIME_DIR=/run/user/\\$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\\$(id -u)/bus $1"; }

if [ -n "$STOP_SERVICE" ]; then
  UNIT="commander-$(basename "$STOP_SERVICE" .py).service"
  usr "systemctl --user disable --now $UNIT" >/dev/null 2>&1 || true
  adb shell "rm -f ~/.config/systemd/user/$UNIT"
  usr "systemctl --user daemon-reload" >/dev/null 2>&1 || true
  echo "$UNIT stopped + removed"
  exit 0
fi

if ! ls bin/*.py >/dev/null 2>&1; then
  echo "nothing in bin/ to deploy — enable a module with SBC tools first (e.g. cmdr module enable ir)."
  exit 0
fi

echo "==> pushing tools to $DEST"
for f in bin/*.py; do
  adb push "$f" "$DEST/$(basename "$f")" >/dev/null && echo "  $(basename "$f")"
done

# Seed the map library once — don't clobber maps you've built on the board (pull those back first).
if [ -d maps ]; then
  if adb shell "test -d $DEST/maps && echo EXISTS" 2>/dev/null | grep -q EXISTS; then
    echo "==> $DEST/maps already on the board — left as-is (adb pull new maps before re-seeding)"
  else
    adb push maps "$DEST/maps" >/dev/null && echo "==> seeded $DEST/maps"
  fi
fi

if [ -n "$SERVICE" ]; then
  TOOL=$(echo "$SERVICE" | awk '{print $1}')
  if [ ! -f "bin/$TOOL" ]; then
    echo "no bin/$TOOL to run as a service (is its module enabled?)" >&2
    exit 1
  fi
  UNIT="commander-$(basename "$TOOL" .py).service"
  echo "==> installing $UNIT (runs at every boot)"
  TMP=$(mktemp)
  cat > "$TMP" <<UNITEOF
[Unit]
Description=commander SBC tool: $SERVICE
# The tool exits when the broker's socket isn't there yet, which is normal during boot — the
# broker is a system service and this is a user one, so they race. Restart=always covers that,
# but ONLY with the start limit lifted: the default (5 starts in 10 s) would give up
# permanently a second or two before the socket appears.
StartLimitIntervalSec=0

[Service]
WorkingDirectory=$DEST
ExecStart=/usr/bin/python3 $DEST/$SERVICE
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNITEOF
  adb shell "mkdir -p ~/.config/systemd/user" >/dev/null
  adb push "$TMP" "$DEST/.config/systemd/user/$UNIT" >/dev/null
  rm -f "$TMP"
  usr "systemctl --user daemon-reload" >/dev/null 2>&1 || true
  usr "systemctl --user enable --now $UNIT" >/dev/null 2>&1 || true
  sleep 2
  STATE=$(usr "systemctl --user is-active $UNIT" 2>/dev/null | tr -d '\\r')
  if [ "$STATE" = "active" ]; then
    echo "  $UNIT active - it will start itself on every boot"
    echo "  logs:  adb shell 'journalctl --user -u $UNIT -f'"
  else
    echo "  $UNIT is '$STATE', not active:" >&2
    usr "journalctl --user -u $UNIT -n 12 --no-pager" >&2
    exit 1
  fi
  exit 0
fi

echo "run them on the board, e.g.:  adb shell 'cd $DEST && python3 ir_lookup.py'"
echo "or run one at every boot:     ./deploy-sbc --service \\"ir_speak.py --greeting\\""
"""


def render(template: str, **kwargs) -> str:
    result = template
    for key, val in kwargs.items():
        result = result.replace(f"__{key.upper()}__", val)
    return result


def write_script(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o755)


def copy_template(name: str, dest: Path) -> None:
    data = importlib.resources.files("cmdr.templates").joinpath(name)
    dest.write_bytes(data.read_bytes())


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def warn(msg: str) -> None:
    """A problem the user must see but that doesn't stop generation — printed to
    stderr so it survives being piped, and prefixed like the in-project `!` notes."""
    print(f"  ! {msg}", file=sys.stderr)


def wifi_credentials() -> tuple[str, str]:
    cfg = load_config()
    ssid     = cfg.get("wifi", "ssid",     fallback="your-network")
    password = cfg.get("wifi", "password", fallback="your-password")
    return ssid, password


# ── Module system (cmdr.toml manifest + generated registration file) ──────────

# Per-platform adapter emitted into the generated file for the Roomba OI link.
ROOMBA_R4_ADAPTER = """\
class R4RoombaPort : public RoombaPort {
public:
    void begin(long baud, int brcPin = -1) {
        _brc = brcPin;
        Serial1.begin(baud);
        if (_brc >= 0) { pinMode(_brc, OUTPUT); digitalWrite(_brc, HIGH); }
    }
    void     write(const uint8_t *d, size_t n) override { Serial1.write(d, n); }
    int      read() override       { return Serial1.read(); }
    int      available() override  { return Serial1.available(); }
    uint32_t now_ms() override     { return millis(); }
    void     delay_ms(uint32_t ms) override { delay(ms); }
    bool     has_brc() const override { return _brc >= 0; }
    void     set_brc(bool high) override { if (_brc >= 0) digitalWrite(_brc, high ? HIGH : LOW); }
private:
    int _brc = -1;
};"""

# Available modules. `always` modules register unconditionally; `questions` is a
# list of (key, prompt, default); `platforms` None means every target.
MODULE_SPECS = {
    "system":  {"always": True,  "platforms": None,   "questions": []},
    "compass": {"always": False, "platforms": None,   "questions": [
        ("sda", "I2C SDA pin", {"pico": "6", "pico2": "6", "esp32": "4", "r4": "4", "uno": "4"}),
        ("scl", "I2C SCL pin", {"pico": "7", "pico2": "7", "esp32": "5", "r4": "5", "uno": "5"}),
    ]},
    "sonar":   {"always": False, "platforms": None,   "questions": [
        ("pin", "Sonar signal pin (PING-style, single pin)", "6"),
    ]},
    "i2c":     {"always": False, "platforms": None,   "questions": [
        ("sda", "I2C SDA pin", {"pico": "6", "pico2": "6", "esp32": "4", "r4": "4", "uno": "4"}),
        ("scl", "I2C SCL pin", {"pico": "7", "pico2": "7", "esp32": "5", "r4": "5", "uno": "5"}),
    ]},
    # INA219 current/power monitor(s) over I2C (Adafruit/SparkFun 0.1 Ω breakout).
    # Multi-instance: `channels` is a comma list of prefix:addr; each gets its own
    # <prefix>volt / <prefix>amp / <prefix>watt / <prefix>init / <prefix>stats
    # commands (the `stats` CSV is what solar-monitor logs). esp32 defaults to GPIO8/9.
    "ina219":  {"always": False, "platforms": None,   "questions": [
        ("sda",      "I2C SDA pin", {"pico": "6", "pico2": "6", "esp32": "8", "r4": "4", "uno": "4"}),
        ("scl",      "I2C SCL pin", {"pico": "7", "pico2": "7", "esp32": "9", "r4": "5", "uno": "5"}),
        ("channels", "INA219 channels as prefix:addr, comma-separated", "a:0x40"),
    ]},
    # WiFi status/control (wifi status|off|on). Runner implements the hooks, so
    # only WiFi platforms whose runner provides them.
    "wifi":    {"always": False, "platforms": ["pico", "pico2", "r4", "esp32"], "questions": []},
    "ir":      {"always": False, "platforms": ["pico", "pico2", "uno", "r4", "unoq", "esp32", "bluepill"], "questions": [
        # unoq sets the pin in app.overlay (ir-gpios), so its value here is informational.
        ("gpio", "IR receive pin", {"pico": "22", "pico2": "22", "uno": "5", "r4": "5", "unoq": "5", "esp32": "38", "bluepill": "0x10"}),
    ], "features": [
        ("wall", "Roomba virtual-wall detection?", False, "COMMANDER_IR_WALL"),
    ], "tools": ["irmap.py", "irlookup.py"],
        # On unoq IR is consumed over the channel bus (ch1), not a serial console — install
        # the socket-based versions (run on the SBC next to the broker; no pyserial/find_port).
        "unoq_tools": ["ir_map.py", "ir_lookup.py", "ir_speak.py", "irchan.py"],
        "seed_dirs": [("maps", "ir_maps")],
        "pio_lib_deps": ["IRremote"]},
    "roomba":  {"always": False, "platforms": ["r4"], "questions": [
        ("baud", "Roomba OI baud rate", "115200"),
        ("brc",  "BRC/wake pin (Mini-DIN 5), -1 if none", "-1"),
    ], "conflicts": ["loco-bridge"]},
    "locomotion": {"always": False, "platforms": ["pico", "pico2"], "questions": [
        ("addr", "Mobile-base bridge I2C address", "0x42"),
        ("sda",  "I2C SDA pin", "6"),
        ("scl",  "I2C SCL pin", "7"),
    ]},
    # Bluetooth game-controller input (Bluepad32 + BTstack). Generic — the module
    # only publishes input (poll/push/bind); the app wires it to anything via the
    # weak commander_on_controller_ready(ControllerModule&) hook or `bind`.
    # Needs BLUEPAD32_PATH + injects CMake (CYW43_ENABLE_BLUETOOTH, the runner option).
    "controller": {"always": False, "platforms": ["pico", "pico2"], "questions": []},
    # Secondary-UART → telnet bridge. Streams whatever is on a hardware UART to the
    # connected telnet (or serial) client. Useful for monitoring another board (e.g.
    # an R4 or ESP32) wired to the Pico's UART1 RX without a second USB connection.
    # uart0 is typically the console UART, so defaults to uart1 (GP8/GP9).
    "serial_monitor": {"always": False, "platforms": ["pico", "pico2"], "questions": [
        ("uart", "UART instance (0 or 1)", "1"),
        ("rx",   "RX pin",                "9"),
        ("tx",   "TX pin",                "8"),
        ("baud", "baud rate",             "115200"),
    ]},
    # R4 side: I2C-slave bridge that forwards CMD_LOCO_* to a Roomba over Serial1.
    # Self-contained — it also provides the `oi` debug command (via RoombaModule on
    # the shared driver), so it supersedes `roomba` on the R4 (mutually exclusive).
    "loco-bridge": {"always": False, "platforms": ["r4"], "questions": [
        ("addr", "This bridge's I2C slave address", "0x42"),
        ("port", "I2C port (0=Wire A4/A5 5V, 1=Wire1 Qwiic 3.3V)", "1"),
        ("baud", "Roomba OI baud rate", "115200"),
        ("brc",  "BRC/wake pin (Mini-DIN 5), -1 if none", "-1"),
    ], "conflicts": ["roomba"]},
    # IPSTube clock: six ST7789 (135x240) IPS displays on one SPI bus, per-display
    # CS, shared backlight. esp_lcd driver; the app drives the displays via the
    # commander_on_ipstube_ready() hook. Pins default to the IPSTube wiring; enable
    # injects COMMANDER_ENABLE_IPSTUBE so the runner pulls esp_lcd + compiles it.
    "ipstube": {"always": False, "platforms": ["esp32"], "questions": [],
                "tools": ["img2rgb565.py"]},
    # Generic WS2812/SK6812 addressable-RGB chain (ESP32 RMT). Command `wled`; the
    # app drives effects via commander_on_ws2812_ready. A board's onboard RGB LED
    # is just this with count=1. Enable injects COMMANDER_ENABLE_WS2812 so the
    # runner compiles it (esp_driver_rmt is already required unconditionally).
    # Two backends behind one module name and one `wled` command: ESP32 drives the
    # chain from RMT, Pico from a PIO state machine (platform/pico/PicoWs2812Module).
    "ws2812": {"always": False, "platforms": ["esp32", "pico", "pico2"], "questions": [
        ("pin",   "WS2812 data GPIO", {"esp32": "5", "pico": "12", "pico2": "12"}),
        ("count", "number of LEDs in the chain", {"esp32": "6", "pico": "1", "pico2": "1"}),
        ("order", "colour order (GRB/RGB/BRG/RBG/GBR/BGR)", "GRB"),
        # These chips are searingly bright at full scale, and how bright is a
        # property of the board, not of the app — so it belongs here with the
        # pin and the colour order. Apps should express colours at full scale and
        # let this set the level; `wled bright <n>` tunes it live.
        ("brightness", "default brightness 0-255", "255"),
    ]},
    # Grove Vision AI Module V2 (WiseEye2 + camera) host module — SSCMA AT protocol.
    # Default UART transport (esp32-specific backend, gated COMMANDER_ENABLE_AICAM);
    # I2C transport is portable/header-only (no gate). The app wires inference results
    # via commander_on_aicam_ready. Wiring is fixed by the XIAO socket — TX43/RX44 for
    # uart, SDA5/SCL6 @ 0x62 for i2c (override in cmdr.toml / -DAICAM_* if needed).
    "aicam": {"always": False, "platforms": ["esp32"], "questions": [
        ("transport", "Vision AI link transport (uart/i2c)", "uart"),
    ]},
    # ── Pico Breadboard Kit peripherals ──────────────────────────────────────
    # These are generic parts, not kit-specific: an ST7796 panel, a GT911 touch
    # layer, an analog stick, buttons, LEDs and a buzzer. Defaults match the
    # GeeekPi Pico Breadboard Kit wiring because that's the board they were
    # brought up on — change the pins and they work anywhere.
    #
    # SPI/ADC/PWM only exist in hal/pico today, so those three are gated to
    # pico/pico2. Widen the platform list at the same time as the HAL, never
    # before — an enable-able module with a stubbed HAL reads as broken hardware.
    "st7796": {"always": False, "platforms": ["pico", "pico2"], "questions": [
        ("sck",      "SPI SCK pin",                   "2"),
        ("mosi",     "SPI MOSI (DIN) pin",            "3"),
        ("cs",       "Chip-select pin",               "5"),
        ("dc",       "Data/command pin",              "6"),
        ("rst",      "Reset pin",                     "7"),
        ("bl",       "Backlight PWM pin (-1 if hard-wired on)", "-1"),
        ("rotation", "Rotation 0-3 (90° per step)",   "0"),
    ], "conflicts": ["st7789"]},
    # ST7789: the controller behind most small colour IPS modules. Same driver
    # base as the ST7796 and the same `lcd` command — which is why the two are
    # declared as conflicting. The `panel` preset picks the glass size *and* the
    # controller RAM behind it, and that pair is what sets the window offset.
    # Defaults are the Waveshare RP2350-GEEK's 1.14" wiring.
    "st7789": {"always": False, "platforms": ["pico", "pico2"], "questions": [
        ("panel",    "Panel (240x135 / 240x240 / 320x170 / 240x320 / custom)", "240x135"),
        ("sck",      "SPI SCK pin",                   "10"),
        ("mosi",     "SPI MOSI (DIN) pin",            "11"),
        ("cs",       "Chip-select pin",               "9"),
        ("dc",       "Data/command pin",              "8"),
        ("rst",      "Reset pin",                     "12"),
        ("bl",       "Backlight PWM pin (-1 if hard-wired on)", "25"),
        ("rotation", "Rotation 0-3 (1 = landscape on the GEEK)", "1"),
    ], "conflicts": ["st7796"]},
    "gt911":  {"always": False, "platforms": ["pico", "pico2", "esp32", "uno", "r4"], "questions": [
        ("sda",      "I2C SDA pin", {"pico": "8", "pico2": "8", "esp32": "4", "r4": "4", "uno": "4"}),
        ("scl",      "I2C SCL pin", {"pico": "9", "pico2": "9", "esp32": "5", "r4": "5", "uno": "5"}),
        ("addr",     "GT911 I2C address (0x5D or 0x14)", "0x5D"),
        ("rotation", "Rotation 0-3 — match the display", "0"),
    ]},
    "joystick": {"always": False, "platforms": ["pico", "pico2"], "questions": [
        ("x",        "X axis ADC pin",                "26"),
        ("y",        "Y axis ADC pin",                "27"),
        ("sw",       "Push-switch pin (-1 if none)",  "-1"),
        ("deadzone", "Deadzone, percent of travel",   "30"),
    ]},
    "buttons": {"always": False, "platforms": None, "questions": [
        ("pins",       "Button pins, comma-separated", {"pico": "15,14", "pico2": "15,14", "esp32": "0", "r4": "2", "uno": "2"}),
        ("active_low", "Pressed reads low (pull-up wiring)?", "yes"),
        ("debounce",   "Debounce window in ms",        "25"),
    ]},
    "leds":   {"always": False, "platforms": None, "questions": [
        ("pins",        "LED pins, comma-separated", {"pico": "16,17", "pico2": "16,17", "esp32": "2", "r4": "13", "uno": "13"}),
        ("active_high", "LED lights when the pin is high?", "yes"),
    ]},
    "buzzer": {"always": False, "platforms": ["pico", "pico2"], "questions": [
        ("pin", "Buzzer PWM pin", "13"),
    ]},
    # DS1302 RTC — Maxim 3-wire, bit-banged over hal_gpio_* (portable, all
    # platforms). Command `rtc`; the app reads/writes time via
    # commander_on_ds1302_ready. Pins default to the IPSTube wiring.
    "ds1302": {"always": False, "platforms": None, "questions": [
        ("sclk", "DS1302 SCLK (clock) pin", "22"),
        ("io",   "DS1302 IO (data) pin",    "19"),
        ("ce",   "DS1302 CE (enable/reset) pin", "21"),
    ]},
}


# The Uno Q's Zephyr HAL backs only UART (console/channel bus) + devicetree IR so far;
# GPIO/I2C are stubbed, so the I2C/GPIO sensor modules would enable but not work. Keep the
# menu honest — only these are offered on unoq until the HAL grows (no broken promises).
UNOQ_MODULES = {"system", "ir"}

# Same honest-menu rule for the Bluepill: hal/stm32 I2C is still stubbed (PLAN
# "Bluepill I2C"), so the I2C-backed modules would enable but read nothing there.
# Shrink this set as the HAL grows. (GPIO/pulse work — sonar and ds1302 are fine.)
BLUEPILL_EXCLUDED = {"compass", "i2c", "ina219"}


def _module_supported(name: str, target: str) -> bool:
    if target == "unoq":
        return name in UNOQ_MODULES
    if target == "bluepill" and name in BLUEPILL_EXCLUDED:
        return False
    plats = MODULE_SPECS[name]["platforms"]
    return plats is None or target in plats


# ST7789 modules show a window of a 240x320 controller RAM, and the size of that
# window is the whole difference between panel variants. Presets rather than four
# raw numbers, because getting the RAM size wrong is invisible until the image is
# offset on the glass. "custom" reads nativeW/nativeH/ramW/ramH from cmdr.toml.
_ST7789_PANELS = {
    "240x135": (135, 240, 240, 320),   # 1.14" — Waveshare RP2350-GEEK / RP2040-GEEK
    "240x240": (240, 240, 240, 320),   # 1.3"
    "320x170": (170, 320, 240, 320),   # 1.9"
    "240x320": (240, 320, 240, 320),   # 2.0" full-frame — no offset
}


def _st7789_geometry(opts: dict):
    """(nativeW, nativeH, ramW, ramH) for the chosen panel preset."""
    panel = str(opts.get("panel", "240x135")).strip().lower()
    if panel in _ST7789_PANELS:
        return _ST7789_PANELS[panel]
    if panel != "custom":
        die(f"st7789 'panel' must be one of {', '.join(_ST7789_PANELS)} or custom "
            f"(got '{panel}')")
    return (opts.get("nativeW", 135), opts.get("nativeH", 240),
            opts.get("ramW", 240),    opts.get("ramH", 320))


def _yes(v) -> bool:
    """Truthiness for a cmdr.toml answer, which may be a bool or a typed word."""
    if isinstance(v, bool):
        return v
    return str(v).strip().lower() in ("yes", "y", "true", "1", "on")


def _pin_list(v, module: str) -> list:
    """Parse a comma-separated pin answer ("15,14") into a list of ints."""
    if isinstance(v, (list, tuple)):
        items = list(v)
    else:
        items = [p for p in str(v).replace(" ", "").split(",") if p]
    if not items:
        die(f"{module}: needs at least one pin")
    pins = []
    for p in items:
        try:
            pins.append(int(str(p), 0))
        except ValueError:
            die(f"{module}: '{p}' is not a pin number")
    if len(pins) > 8:
        die(f"{module}: at most 8 pins (got {len(pins)})")
    return pins


def _emit_module(name: str, opts: dict, target: str):
    """Return (includes, decls, registers, tickers) C++ fragments for one module.

    `tickers` are uart.addTicker() calls for modules whose tick() must be pumped
    by the UART task (e.g. IR recv mode). They go into a generated
    commander_on_uart_ready() hook.
    """
    if name == "system":
        return (['#include "core/SystemModule.h"'],
                ["static SystemModule _m_system;"],
                ["reg.registerModule(_m_system);"], [])
    if name == "compass":
        sda = opts.get("sda", 4)
        scl = opts.get("scl", 5)
        # Compass uses the global HAL I2C bus; bring it up here since the
        # default CommanderConfig leaves I2C off (i2c_sda = -1).
        return (['#include "hal/hal.h"', '#include "modules/CompassModule.h"'],
                ["static CompassModule _m_compass;"],
                [f"hal_i2c_init({sda}, {scl}, 100000);",
                 "reg.registerModule(_m_compass);"], [])
    if name == "sonar":
        pin = opts.get("pin", 6)
        return (['#include "modules/SonarModule.h"'],
                [f"static SonarModule _m_sonar({pin});"],
                ["reg.registerModule(_m_sonar);"], [])
    if name == "i2c":
        # Bus diagnostics (scan/read/write). Brings up the global HAL I2C bus,
        # like compass; the hal_i2c_init line is deduped if another I2C module
        # (compass/locomotion) already emitted the same one.
        sda = opts.get("sda", 4)
        scl = opts.get("scl", 5)
        return (['#include "hal/hal.h"', '#include "modules/I2CDiagModule.h"'],
                ["static I2CDiagModule _m_i2c;"],
                [f"hal_i2c_init({sda}, {scl}, 100000);",
                 "reg.registerModule(_m_i2c);"], [])
    if name == "wifi":
        return (['#include "modules/WifiModule.h"'],
                ["static WifiModule _m_wifi;"],
                ["reg.registerModule(_m_wifi);"], [])
    if name == "ipstube":
        if target != "esp32":
            die(f"ipstube module is not supported on target '{target}'")
        # esp_lcd driver lives in platform/esp32 and compiles in the runner under
        # COMMANDER_ENABLE_IPSTUBE (set by _ipstube_cmake_enable). The app gets a
        # reference via the weak commander_on_ipstube_ready hook (forward-declared).
        return (['#include "platform/esp32/IpstubeModule.h"'],
                ["static IpstubeModule _m_ipstube;",
                 "void commander_on_ipstube_ready(IpstubeModule &);"],
                ["reg.registerModule(_m_ipstube);",
                 "commander_on_ipstube_ready(_m_ipstube);"], [])
    if name == "ws2812":
        if target not in ("esp32", "pico", "pico2"):
            die(f"ws2812 module is not supported on target '{target}'")
        pin   = opts.get("pin", 5 if target == "esp32" else 12)
        count = opts.get("count", 6 if target == "esp32" else 1)
        order = str(opts.get("order", "GRB")).strip().upper()
        if order not in ("GRB", "RGB", "BRG", "RBG", "GBR", "BGR"):
            die(f"ws2812 'order' must be one of GRB/RGB/BRG/RBG/GBR/BGR (got '{order}')")
        bright = int(opts.get("brightness", 255))
        if not 0 <= bright <= 255:
            die(f"ws2812 'brightness' must be 0..255 (got {bright})")
        # Only emitted when it differs from the module's own default, so existing
        # projects' generated files don't churn on regen.
        dim = [f"_m_ws2812.setBrightness({bright});"] if bright != 255 else []
        if target in ("pico", "pico2"):
            # PIO backend; the commander_pico_ws2812 target (linked into the pico
            # runner) owns the .pio build, so enabling is pure registration.
            return (['#include "platform/pico/PicoWs2812Module.h"'],
                    [f"static PicoWs2812Module _m_ws2812({pin}, {count}, PicoWs2812Module::{order});"],
                    ["reg.registerModule(_m_ws2812);"] + dim +
                    ["if (commander_on_ws2812_ready) commander_on_ws2812_ready(_m_ws2812);"], [])
        return (['#include "platform/esp32/Ws2812Module.h"'],
                [f"static Ws2812Module _m_ws2812({pin}, {count}, Ws2812Module::{order});",
                 "void commander_on_ws2812_ready(Ws2812Module &);"],
                ["reg.registerModule(_m_ws2812);"] + dim +
                ["commander_on_ws2812_ready(_m_ws2812);"], [])
    if name == "aicam":
        if target != "esp32":
            die(f"aicam module is not supported on target '{target}'")
        # Streaming results are pumped by the UART task, so add a ticker. The hook
        # is a weak *declaration* in AiCamModule.h (header-only), so null-check it.
        transport = str(opts.get("transport", "uart")).strip().lower()
        if transport == "i2c":
            # Portable I2C backend (Grove Vision AI V2 @ 0x62). Brings up the HAL
            # I2C bus (deduped against compass/i2c if the pins match).
            sda  = opts.get("sda", 5)
            scl  = opts.get("scl", 6)
            addr = opts.get("addr", 0x62)
            addr_lit = f"0x{addr:02X}" if isinstance(addr, int) else addr
            return (['#include "hal/hal.h"',
                     '#include "modules/aicam/I2cTransport.h"',
                     '#include "modules/aicam/AiCamModule.h"'],
                    [f"static AiCamI2cTransport _t_aicam({addr_lit});",
                     "static AiCamModule _m_aicam(_t_aicam);"],
                    [f"hal_i2c_init({sda}, {scl}, 400000);",
                     "reg.registerModule(_m_aicam);",
                     "if (commander_on_aicam_ready) commander_on_aicam_ready(_m_aicam);"],
                    ["uart.addTicker(_m_aicam);"])
        if transport != "uart":
            die(f"aicam 'transport' must be 'uart' or 'i2c' (got '{transport}')")
        # esp32 UART backend (compiled in the runner under COMMANDER_ENABLE_AICAM).
        tx = opts.get("tx", 43)
        rx = opts.get("rx", 44)
        return (['#include "platform/esp32/AiCamUartTransport.h"',
                 '#include "modules/aicam/AiCamModule.h"'],
                [f"static AiCamUartTransport _t_aicam({tx}, {rx});",
                 "static AiCamModule _m_aicam(_t_aicam);"],
                ["reg.registerModule(_m_aicam);",
                 "if (commander_on_aicam_ready) commander_on_aicam_ready(_m_aicam);"],
                ["uart.addTicker(_m_aicam);"])
    if name in ("st7796", "st7789"):
        # SPI TFT panels. Both are SpiPanel subclasses differing only in bring-up
        # and MADCTL, so they share one emitter — and one `lcd` command, which is
        # why MODULE_SPECS marks them as conflicting.
        sck  = opts.get("sck", 10 if name == "st7789" else 2)
        mosi = opts.get("mosi", 11 if name == "st7789" else 3)
        cs   = opts.get("cs", 9 if name == "st7789" else 5)
        dc   = opts.get("dc", 8 if name == "st7789" else 6)
        rst  = opts.get("rst", 12 if name == "st7789" else 7)
        bl   = opts.get("bl", 25 if name == "st7789" else -1)
        rot  = int(opts.get("rotation", 1 if name == "st7789" else 0)) & 3
        # Which SPI controller the pins belong to is fixed by the chip's pinmux:
        # on RP2040/RP2350, SCK 10/14/26 are spi1, everything else spi0.
        bus  = 1 if int(sck) in (10, 14, 26) else 0
        hz   = opts.get("hz", 40000000)
        inv  = "true" if _yes(opts.get("invert", True)) else "false"

        if name == "st7789":
            nw, nh, rw, rh = _st7789_geometry(opts)
            cls, hdr = "St7789Module", "modules/display/St7789Module.h"
        else:
            nw = opts.get("width", 320)
            nh = opts.get("height", 480)
            rw = rh = 0                    # RAM == glass, so no window offset
            cls, hdr = "St7796Module", "modules/display/St7796Module.h"

        # Emitted field-by-field rather than as a positional aggregate: adding a
        # field to SpiPanelConfig would silently shift every value after it, and
        # a display whose rotation lands in `ramW` is a bad afternoon.
        var = f"_c_{name}"
        decl = [f"static const SpiPanelConfig {var} = [] {{",
                "    SpiPanelConfig c;",
                f"    c.bus = {bus}; c.sck = {sck}; c.mosi = {mosi};",
                f"    c.cs = {cs}; c.dc = {dc}; c.rst = {rst}; c.bl = {bl};",
                f"    c.nativeW = {nw}; c.nativeH = {nh};",
                f"    c.ramW = {rw}; c.ramH = {rh};",
                f"    c.rotation = {rot}; c.hz = {hz}; c.invert = {inv};",
                "    return c;",
                "}();",
                f"static {cls} _m_{name}({var});"]
        return ([f'#include "{hdr}"'],
                decl,
                [f"reg.registerModule(_m_{name});",
                 f"if (commander_on_display_ready) commander_on_display_ready(_m_{name});"], [])
    if name == "gt911":
        # Capacitive touch. Brings up the global HAL I2C bus (deduped against
        # compass/i2c when the pins match) and is pumped by the UART task.
        sda  = opts.get("sda", 8)
        scl  = opts.get("scl", 9)
        addr = opts.get("addr", 0x5D)
        addr_lit = f"0x{addr:02X}" if isinstance(addr, int) else addr
        rot  = int(opts.get("rotation", 0)) & 3
        # 100 kHz, matching every other I2C module here — so the shared-bus line
        # dedupes with theirs instead of re-initialising the bus at a second speed.
        # The GT911 is specified to 400 kHz but the original vendor driver ran this
        # panel at 100 kHz, which is the rate this wiring is known good at.
        return (['#include "hal/hal.h"', '#include "modules/touch/Gt911Module.h"'],
                [f"static Gt911Module _m_gt911({addr_lit}, {rot});"],
                [f"hal_i2c_init({sda}, {scl}, 100000);",
                 "reg.registerModule(_m_gt911);",
                 "if (commander_on_touch_ready) commander_on_touch_ready(_m_gt911);"],
                ["uart.addTicker(_m_gt911);"])
    if name == "joystick":
        x  = opts.get("x", 26)
        y  = opts.get("y", 27)
        sw = opts.get("sw", -1)
        dz = int(opts.get("deadzone", 30))
        return (['#include "modules/input/JoystickModule.h"'],
                [f"static JoystickModule _m_joystick({x}, {y}, {sw}, {dz});"],
                ["reg.registerModule(_m_joystick);",
                 "if (commander_on_joystick_ready) commander_on_joystick_ready(_m_joystick);"],
                ["uart.addTicker(_m_joystick);"])
    if name == "buttons":
        pins = _pin_list(opts.get("pins", "15,14"), "buttons")
        low  = "true" if _yes(opts.get("active_low", True)) else "false"
        deb  = int(opts.get("debounce", 25))
        return (['#include "modules/input/ButtonsModule.h"'],
                [f"static const uint8_t _p_buttons[] = {{{', '.join(str(p) for p in pins)}}};",
                 f"static ButtonsModule _m_buttons(_p_buttons, {len(pins)}, {low}, {deb});"],
                ["reg.registerModule(_m_buttons);",
                 "if (commander_on_buttons_ready) commander_on_buttons_ready(_m_buttons);"],
                ["uart.addTicker(_m_buttons);"])
    if name == "leds":
        pins = _pin_list(opts.get("pins", "16,17"), "leds")
        high = "true" if _yes(opts.get("active_high", True)) else "false"
        return (['#include "modules/LedModule.h"'],
                [f"static const uint8_t _p_leds[] = {{{', '.join(str(p) for p in pins)}}};",
                 f"static LedModule _m_leds(_p_leds, {len(pins)}, {high});"],
                ["reg.registerModule(_m_leds);",
                 "if (commander_on_leds_ready) commander_on_leds_ready(_m_leds);"],
                ["uart.addTicker(_m_leds);"])
    if name == "buzzer":
        pin = opts.get("pin", 13)
        return (['#include "modules/BuzzerModule.h"'],
                [f"static BuzzerModule _m_buzzer({pin});"],
                ["reg.registerModule(_m_buzzer);",
                 "if (commander_on_buzzer_ready) commander_on_buzzer_ready(_m_buzzer);"],
                ["uart.addTicker(_m_buzzer);"])
    if name == "ds1302":
        sclk = opts.get("sclk", 22)
        io   = opts.get("io", 19)
        ce   = opts.get("ce", 21)
        # Header-only module: the hook is a weak *declaration* (see the header),
        # so null-check before calling — no forward-decl, no weak definition to host.
        return (['#include "modules/Ds1302Module.h"'],
                [f"static Ds1302Module _m_ds1302({sclk}, {io}, {ce});"],
                ["reg.registerModule(_m_ds1302);",
                 "if (commander_on_ds1302_ready) commander_on_ds1302_ready(_m_ds1302);"], [])
    if name == "ina219":
        # One namespaced `ina` command for however many INA219 sensors are wired;
        # `channels` is a comma list of label:addr. Brings up the global HAL I2C
        # bus (deduped against compass/i2c if the pins match).
        sda = opts.get("sda", 8)
        scl = opts.get("scl", 9)
        regs = [f"hal_i2c_init({sda}, {scl}, 100000);"]
        for ch in str(opts.get("channels", "a:0x40")).split(","):
            ch = ch.strip()
            if not ch:
                continue
            prefix, _, addr = ch.partition(":")
            prefix = prefix.strip() or "a"
            addr = addr.strip() or "0x40"
            regs.append(f'_m_ina219.addChannel("{prefix}", {addr});')
        regs.append("reg.registerModule(_m_ina219);")
        return (['#include "hal/hal.h"', '#include "modules/Ina219Module.h"'],
                ["static Ina219Module _m_ina219;"], regs, [])
    if name == "ir":
        # Platform-specific (PIO on Pico). The commander_pico_ir CMake target
        # (linked into pico_runner) provides the PIO build + clean header. IR
        # prints decoded codes from tick(), so it must be added as a UART ticker.
        if target in ("pico", "pico2"):
            gpio = opts.get("gpio", 22)
            return (['#include "platform/pico/PicoIRModule.h"'],
                    [f"static PicoIRModule _m_ir({gpio});"],
                    ["reg.registerModule(_m_ir);"],
                    ["uart.addTicker(_m_ir);"])
        if target in ("uno", "r4"):
            # Arduino IRremote-based (Uno + R4). Unity-include the .cpp (like the
            # runner does for ArduinoTelnetTransport.cpp) so it compiles in the
            # app TU; IRremote comes from the pio_lib_deps added on enable.
            pin = opts.get("gpio", 5)
            return (['#include "platform/arduino/IRModule.h"',
                     '#include "platform/arduino/IRModule.cpp"'],
                    [f"static IRModule _m_ir({pin});"],
                    ["reg.registerModule(_m_ir);"],
                    ["uart.addTicker(_m_ir);"])
        if target == "unoq":
            # Zephyr GPIO-ISR NEC/Sony receiver (pin from app.overlay's ir-gpios). On the
            # channel-bus build it publishes each press on CH_IR and is pumped by the bus
            # thread — so its wiring (publisher + ticker) goes into the channel-bus-ready
            # hook, where `bus` is in scope (see generate_modules_file). The channel id
            # comes from channel_ids.h (the authority), never a hand-picked integer.
            return (['#include "channel_ids.h"',
                     '#include "platform/zephyr/ZephyrIRModule.h"',
                     '#include "transport/channels/ChannelBusRunner.h"'],
                    ["static ZephyrIRModule _m_ir;",
                     "static ChannelTransport::ChannelPublisher _pub_ir;"],
                    ["reg.registerModule(_m_ir);"],
                    ["_pub_ir = bus.channels().publisher(CH_IR);",
                     "_m_ir.setOutput(&_pub_ir);",
                     "bus.addTicker(_m_ir);"])
        if target == "esp32":
            # ESP32 RMT-based receiver. Compiled in the runner under COMMANDER_ENABLE_IR
            # (set by _ir_esp32_cmake_enable on enable). esp_driver_rmt is already required
            # unconditionally by the runner so no REQUIRES change is needed.
            gpio = opts.get("gpio", 38)
            return (['#include "platform/esp32/Esp32IRModule.h"'],
                    [f"static Esp32IRModule _m_ir({gpio});"],
                    ["reg.registerModule(_m_ir);"],
                    ["uart.addTicker(_m_ir);"])
        if target == "bluepill":
            # STM32F103 EXTI edge interrupt + DWT timing. Unity-included like the Arduino
            # IRModule so the ISR handlers compile into the app TU without a separate build
            # entry — the Bluepill PlatformIO build lists sources explicitly.
            gpio = opts.get("gpio", 16)
            return (['#include "platform/stm32-bluepill/Stm32IRModule.h"',
                     '#include "platform/stm32-bluepill/Stm32IRModule.cpp"'],
                    [f"static Stm32IRModule _m_ir({gpio});"],
                    ["reg.registerModule(_m_ir);"],
                    ["uart.addTicker(_m_ir);"])
        die(f"ir module is not yet supported on target '{target}'")
    if name == "roomba":
        baud = opts.get("baud", 115200)
        brc  = opts.get("brc", -1)
        return (['#include <Arduino.h>', '#include "modules/roomba/RoombaModule.h"'],
                [ROOMBA_R4_ADAPTER,
                 "static R4RoombaPort _m_roomba_port;",
                 "static RoombaModule _m_roomba(_m_roomba_port);"],
                [f"_m_roomba_port.begin({baud}, {brc});",
                 "reg.registerModule(_m_roomba);"], [])
    if name == "locomotion":
        # Master side: drives a remote base over I2C (e.g. the R4 Roomba bridge).
        # Brings up the global HAL I2C bus here, like compass, since the default
        # CommanderConfig leaves I2C off (i2c_sda = -1).
        addr = opts.get("addr", 0x42)
        sda  = opts.get("sda", 4)
        scl  = opts.get("scl", 5)
        return (['#include "hal/hal.h"', '#include "modules/locomotion/LocomotionModule.h"'],
                [f"static LocomotionModule _m_locomotion({addr});"],
                [f"hal_i2c_init({sda}, {scl}, 100000);",
                 "reg.registerModule(_m_locomotion);"], [])
    if name == "loco-bridge":
        # R4 I2C-slave bridge. Owns the shared Roomba stack (so it also provides
        # `oi`) and adds the I2C slave layer. Its tick() does the blocking Roomba
        # I/O, so it must be pumped by the UART task (uart.addTicker).
        addr = opts.get("addr", 0x42)
        baud = opts.get("baud", 115200)
        brc  = opts.get("brc", -1)
        # I2C port: Wire1 (Qwiic, 3.3V — direct to a Pico master) or Wire (A4/A5,
        # 5V — needs a level shifter). Both are IIC peripherals, so both do slave.
        wire = "Wire1" if int(opts.get("port", 1)) else "Wire"
        return (['#include <Arduino.h>',
                 '#include "modules/roomba/RoombaModule.h"',
                 '#include "runners/arduino-r4/LocomotionBridge.h"'],
                [ROOMBA_R4_ADAPTER,
                 "static R4RoombaPort _m_roomba_port;",
                 "static RoombaModule _m_roomba(_m_roomba_port);",
                 f"static LocomotionBridge _m_loco_bridge(_m_roomba.driver(), {addr}, {wire});"],
                [f"_m_roomba_port.begin({baud}, {brc});",
                 "reg.registerModule(_m_roomba);",
                 "reg.registerModule(_m_loco_bridge);"],
                ["uart.addTicker(_m_loco_bridge);"])
    if name == "controller":
        if target not in ("pico", "pico2"):
            die(f"controller module is not supported on target '{target}'")
        # Generic input module + Bluepad32 backend. After registering, call the
        # weak app hook so the app can add onUpdate/onButton listeners or bindings
        # (e.g. map sticks to drive). The hook's weak default lives in the runner.
        return (['#include "platform/pico/PicoBluepadBackend.h"'],
                ["static PicoBluepadBackend _m_controller_backend;",
                 "static ControllerModule _m_controller(_m_controller_backend);",
                 "void commander_on_controller_ready(ControllerModule &);"],
                ["reg.registerModule(_m_controller);",
                 "commander_on_controller_ready(_m_controller);"], [])
    if name == "serial_monitor":
        if target not in ("pico", "pico2"):
            die(f"serial_monitor module is not supported on target '{target}'")
        uart_num = int(opts.get("uart", 1))
        if uart_num not in (0, 1):
            die(f"serial_monitor 'uart' must be 0 or 1 (got '{uart_num}')")
        rx   = opts.get("rx",   9)
        tx   = opts.get("tx",   8)
        baud = opts.get("baud", 115200)
        return (['#include "runners/pico/SerialMonitorModule.h"'],
                [f"static SerialMonitorModule _m_serial_monitor(uart{uart_num}, {rx}, {tx}, {baud});"],
                ["reg.registerModule(_m_serial_monitor);"], [])
    die(f"no code emitter for module '{name}'")


def _c_str_literal(s: str) -> str:
    """Escape a string for embedding as a C string literal (autostart commands)."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def generate_modules_file(target: str, modules: dict, out_path: Path,
                          autostart: "list" = None) -> None:
    autostart = autostart or []
    _i2c_claims: dict = {}          # (sda, scl) -> modules that asked for it
    # system always first, then enabled optional modules alphabetically.
    order = ["system"] + sorted(m for m in modules if m != "system")
    includes: list = []
    decls: list = []
    registers: list = []
    tickers: list = []
    for name in order:
        inc, dec, reg, tick = _emit_module(name, modules.get(name, {}), target)
        for i in inc:
            if i not in includes:
                includes.append(i)
        decls += dec
        # Dedupe identical register lines so two I2C modules (e.g. compass/
        # locomotion/i2c) don't each emit hal_i2c_init for the one shared HAL bus.
        for r in reg:
            if r not in registers:
                registers.append(r)
        tickers += tick
        for r in reg:
            m = re.match(r"hal_i2c_init\((\d+),\s*(\d+),", r)
            if m:
                _i2c_claims.setdefault((int(m.group(1)), int(m.group(2))), []).append(name)

    # The HAL owns ONE I2C bus, so two modules asking for different pins is not a
    # composition — the last hal_i2c_init() wins and the other module's device
    # goes silent. That reads as dead hardware, so say it here, at the moment the
    # file is generated, rather than leaving it to be debugged on a bench.
    if len(_i2c_claims) > 1:
        warn("enabled modules disagree about the I2C pins — the HAL has one bus, "
             "so the last one wins and the others will not respond:")
        for (sda, scl), owners in _i2c_claims.items():
            warn(f"    SDA {sda} / SCL {scl}: {', '.join(sorted(set(owners)))}")
        warn("  Fix: re-enable them with matching pins, or edit cmdr.toml and "
             "`cmdr regen`.")

    # Modules that need tick() pumped (e.g. IR recv) get a strong, non-inline ticker hook
    # that overrides the runner's weak default. The hook + transport type differ by build:
    # the channel-bus runner (unoq) uses commander_on_channel_bus_ready(ChannelBusRunner&),
    # everyone else commander_on_uart_ready(UartTransport&).
    bus_build = (target == "unoq")
    if tickers:
        hdr = ('#include "transport/channels/ChannelBusRunner.h"' if bus_build
               else '#include "transport/uart/UartTransport.h"')
        if hdr not in includes:
            includes.append(hdr)
    if autostart and '#include "core/NullWriter.h"' not in includes:
        includes.append('#include "core/NullWriter.h"')

    lines = [
        "// AUTO-GENERATED by cmdr — do not edit.",
        "// Manage modules with: cmdr module enable|disable|list",
        "#pragma once",
        '#include "core/CommandRegistry.h"',
        *includes,
        "",
        *decls,
        "",
        "inline void commander_register_modules(CommandRegistry &reg) {",
        *["    " + r for r in registers],
        "}",
    ]
    if tickers:
        # The generated hook is a STRONG definition, so an app can no longer
        # define commander_on_uart_ready itself (duplicate symbol) once any
        # ticking module is enabled. Apps still need their own periodic work — a
        # UI refresh, a control loop — so the generated hook ends by calling a
        # weak app hook. Define commander_on_app_tickers() in your main.cpp to
        # add tickers of your own; apps that don't pay nothing.
        pump = "bus" if bus_build else "uart"
        ttype = "ChannelBusRunner" if bus_build else "UartTransport"
        hook = (f'extern "C" void commander_on_channel_bus_ready({ttype} &bus) {{'
                if bus_build else
                f'extern "C" void commander_on_uart_ready({ttype} &uart) {{')
        lines += ["",
                  f'extern "C" void commander_on_app_tickers({ttype} &) __attribute__((weak));',
                  "",
                  hook,
                  *["    " + t for t in tickers],
                  f"    if (commander_on_app_tickers) commander_on_app_tickers({pump});",
                  "}"]
    if autostart:
        # Boot commands dispatched once at startup (cmdr autostart). Output is discarded —
        # we want the side effect (e.g. `ir recv` starting the stream), not the reply. The
        # runner calls this after the ready-hook, so a started stream is already wired.
        lines += ["",
                  'extern "C" void commander_run_autostart(CommandRegistry &reg) {',
                  "    NullWriter _null;",
                  *[f'    reg.dispatch("{_c_str_literal(c)}", _null);' for c in autostart],
                  "}"]
    out_path.write_text("\n".join(lines) + "\n")


# ── cmdr.toml manifest (minimal, dependency-free for our controlled schema) ───

def read_manifest(path: Path):
    """Returns (target, modules, autostart). autostart is an ordered list of boot command
    lines from the [autostart] section (managed by `cmdr autostart`)."""
    target = None
    modules: dict = {}
    autostart: list = []
    cur = None
    in_autostart = False
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[module."):
            cur = modules.setdefault(line[len("[module."):].rstrip("]"), {})
            in_autostart = False
            continue
        if line.startswith("[autostart]"):
            cur = None
            in_autostart = True
            continue
        if line.startswith("["):
            cur = None
            in_autostart = False
            continue
        if "=" in line:
            k, _, v = line.partition("=")
            k, v = k.strip(), v.strip()
            if v[:1] == '"' and v[-1:] == '"':
                val = v[1:-1]
            elif v == "true":
                val = True
            elif v == "false":
                val = False
            else:
                try:
                    val = int(v, 0)
                except ValueError:
                    val = v
            if in_autostart:
                autostart.append(str(val))      # ordered by file order (cmd0, cmd1, …)
            elif cur is None and k == "target":
                target = val
            elif cur is not None:
                cur[k] = val
    return target, modules, autostart


def write_manifest(path: Path, target: str, modules: dict, autostart: "list" = None) -> None:
    lines = [f'target = "{target}"', ""]
    for name in sorted(modules):
        lines.append(f"[module.{name}]")
        for k, v in modules[name].items():
            if isinstance(v, bool):                       # bool before int (bool is an int)
                lines.append(f"{k} = {'true' if v else 'false'}")
            elif isinstance(v, int):
                lines.append(f"{k} = {v}")
            else:
                lines.append(f'{k} = "{v}"')
        lines.append("")
    if autostart:
        lines.append("[autostart]")
        for i, cmd in enumerate(autostart):
            lines.append(f'cmd{i} = "{cmd}"')
        lines.append("")
    path.write_text("\n".join(lines).rstrip() + "\n")


def detect_target() -> str:
    pio = Path("platformio.ini")
    if pio.exists():
        txt = pio.read_text()
        if "uno_r4_wifi" in txt:
            return "r4"
        if "board = uno" in txt or "board=uno" in txt:
            return "uno"
        if "bluepill_f103c8" in txt:
            return "bluepill"
    cmake = Path("CMakeLists.txt")
    if cmake.exists():
        txt = cmake.read_text()
        if "IDF_PATH" in txt:
            return "esp32"
        m = re.search(r"PICO_BOARD\s+(\S+)", txt)
        if m:
            return "pico2" if "pico2" in m.group(1) else "pico"
        if "pico_sdk" in txt:
            return "pico"
    return None


# Where the generated commander_modules.h lives, per target's project layout.
def _modules_file_path(target: str) -> Path:
    if target == "esp32":
        return Path("main") / "commander_modules.h"      # main/main.cpp
    if target in ("pico", "pico2"):
        return Path("commander_modules.h")               # main.cpp at root
    return Path("src") / "commander_modules.h"           # r4 / uno


def _regenerate(target: str, modules: dict, autostart: "list" = None) -> None:
    out = _modules_file_path(target)
    if not out.parent.exists():
        die(f"expected {out.parent}/ directory — run from your project root")
    generate_modules_file(target, modules, out, autostart)
    print(f"regenerated {out}")


# Companion host tooling a module ships (e.g. IR's irmap/irlookup). Tools go in
# bin/ (executable); tool_dirs are data dirs created empty (e.g. maps/).
def _install_tools(spec: dict, target: str) -> None:
    # unoq's tools are channel-bus (socket) versions that run on the SBC — no serial port
    # detection, no pyserial. Other targets get the serial tools + find_port.py.
    serial_tools = target != "unoq"
    tools = spec.get("tools", []) if serial_tools else spec.get("unoq_tools", [])
    if tools:
        bin_dir = Path("bin")
        bin_dir.mkdir(exist_ok=True)
        if serial_tools:
            # Shared port detection so tools pick the right board by VID/PID (same
            # as the monitor script), not just the first cu.usb* device.
            copy_template("find_port.py", bin_dir / "find_port.py")
            (bin_dir / "find_port.py").chmod(0o755)
        for tool in tools:
            dest = bin_dir / tool
            copy_template(tool, dest)
            dest.chmod(0o755)
            print(f"  • bin/{tool}")
        print("    (the IR tools need: pip install pyserial)" if serial_tools else
              "    (run these on the SBC next to the broker — see README)")
    for d in spec.get("tool_dirs", []):
        Path(d).mkdir(exist_ok=True)
        print(f"  • {d}/")
    # Seed data dirs from template subdirs (e.g. IR's button-map library).
    # Never clobber existing files — the user's own maps win.
    for dest_dir, tmpl_subdir in spec.get("seed_dirs", []):
        dest = Path(dest_dir)
        dest.mkdir(exist_ok=True)
        src = importlib.resources.files("cmdr.templates").joinpath(tmpl_subdir)
        seeded = 0
        for entry in src.iterdir():
            if not entry.name.endswith(".json"):
                continue
            target = dest / entry.name
            if not target.exists():
                target.write_bytes(entry.read_bytes())
                seeded += 1
        print(f"  • {dest_dir}/ ({seeded} library map(s))")


def _remove_tools(spec: dict, target: str) -> None:
    bin_dir = Path("bin")
    tools = spec.get("tools", []) if target != "unoq" else spec.get("unoq_tools", [])
    for tool in tools:
        p = bin_dir / tool
        if p.exists():
            p.unlink()
            print(f"  • removed bin/{tool}")
    # Drop the shared find_port.py (and bin/ itself) only if no other tools
    # remain. Leave tool_dirs (e.g. maps/) — they may hold user data.
    if bin_dir.is_dir():
        others = [f for f in bin_dir.iterdir() if f.name != "find_port.py"]
        if not others:
            fp = bin_dir / "find_port.py"
            if fp.exists():
                fp.unlink()
            if not any(bin_dir.iterdir()):
                bin_dir.rmdir()


# PlatformIO lib_deps a module needs (e.g. IR's IRremote). No-op on CMake
# (Pico/ESP32) projects, which have no platformio.ini.
def _add_pio_lib_deps(libs: list) -> None:
    pio = Path("platformio.ini")
    if not pio.exists() or not libs:
        return
    text = pio.read_text()
    out = []
    for line in text.splitlines():
        out.append(line)
        if line.strip().startswith("lib_deps"):
            for lib in libs:
                if lib not in text:
                    out.append(f"    {lib}")
                    print(f"  • platformio.ini lib_deps += {lib}")
    pio.write_text("\n".join(out) + "\n")


def _remove_pio_lib_deps(libs: list) -> None:
    pio = Path("platformio.ini")
    if not pio.exists() or not libs:
        return
    text = pio.read_text()
    for lib in libs:
        text = re.sub(rf"\n[ \t]*{re.escape(lib)}[^\n]*", "", text)
    pio.write_text(text)


# Optional per-module features gated by a build flag. The flag must be set for
# the whole build (consistent across translation units — on Pico the IR header
# is compiled in two TUs, so an inconsistent define would be an ODR violation),
# so it goes in PlatformIO build_flags or a CMake add_compile_definitions().
def _feature_flags_on(modules: dict) -> set:
    on = set()
    for name, opts in modules.items():
        for key, _prompt, _default, flag in MODULE_SPECS.get(name, {}).get("features", []):
            if opts.get(key):
                on.add(flag)
    return on


def _sync_feature_flags(flags_on: set) -> None:
    """Make the project's build flags exactly match `flags_on` (add the enabled
    feature flags, strip any disabled ones). Handles PlatformIO and CMake."""
    all_flags = {f[3] for spec in MODULE_SPECS.values() for f in spec.get("features", [])}
    pio = Path("platformio.ini")
    cmake = Path("CMakeLists.txt")
    if pio.exists():
        text = pio.read_text()
        for fl in all_flags:                                   # strip all, re-add the ON ones
            text = re.sub(rf"\n[ \t]*-D{re.escape(fl)}\b[^\n]*", "", text)
        out = []
        for line in text.splitlines():
            out.append(line)
            if line.strip().startswith("build_flags"):
                out += [f"    -D{fl}" for fl in sorted(flags_on)]
        pio.write_text("\n".join(out) + "\n")
    elif cmake.exists():
        text = cmake.read_text()
        for fl in all_flags:
            text = re.sub(rf"add_compile_definitions\({re.escape(fl)}\)\n", "", text)
        if flags_on and "FetchContent_MakeAvailable(commander)" in text:
            # Before MakeAvailable so it reaches commander's targets and the app.
            adds = "".join(f"add_compile_definitions({fl})\n" for fl in sorted(flags_on))
            text = text.replace("FetchContent_MakeAvailable(commander)",
                                adds + "FetchContent_MakeAvailable(commander)", 1)
        cmake.write_text(text)


# Commands each module registers — used to size MAX_COMMANDS exactly so the
# registry's fixed command array isn't over-allocated (it matters on RAM-tight
# boards like the R4, where every slot is ~20 bytes). The registry uses a fixed
# array (no heap) for deterministic RAM; the count isn't known to the core (modules
# register at runtime), but cmdr knows the enabled set, so it computes it here.
_MODULE_COMMANDS = {
    "system": 2, "compass": 1, "sonar": 1, "i2c": 1, "ir": 1,
    "roomba": 1, "locomotion": 4, "loco-bridge": 1, "controller": 5, "wifi": 1,
    "serial_monitor": 1,
    # ina219 is namespaced: one `ina` command no matter how many channels.
    "ina219": 1,
    # ipstube: one namespaced `ipstube` command (on/off/dim/fill/clear/test).
    "ipstube": 1,
    # ws2812: one `wled` command.
    "ws2812": 1,
    # ds1302: one `rtc` command.
    "ds1302": 1,
    # aicam: one namespaced `aicam` command.
    "aicam": 1,
    # Breadboard-kit peripherals: one namespaced command each —
    # `lcd`, `touch`, `joy`, `btn`, `led`, `buzz`.
    "st7796": 1, "st7789": 1, "gt911": 1, "joystick": 1, "buttons": 1, "leds": 1, "buzzer": 1,
}
# Headroom for runner-registered commands (bootsel on pico, ota when enabled) plus
# a few app-registered ones. Conservative, since under-sizing silently drops commands.
_MAX_COMMANDS_RESERVE = 5


def _compute_max_commands(modules: dict) -> int:
    total = _MODULE_COMMANDS["system"]
    for name in modules:
        total += _MODULE_COMMANDS.get(name, 1)
    return total + _MAX_COMMANDS_RESERVE


def _read_max_commands(root: Path = Path(".")) -> "int | None":
    """The MAX_COMMANDS currently set in the project, if any."""
    for f in (root / "platformio.ini", root / "CMakeLists.txt"):
        if f.exists():
            m = re.search(r"MAX_COMMANDS=(\d+)", f.read_text())
            if m:
                return int(m.group(1))
    return None


def _sync_max_commands(modules: dict, root: Path = Path(".")) -> None:
    """Inject -DMAX_COMMANDS=<count> so the registry array fits the enabled set.

    Only ever grows. The computed value covers cmdr-managed modules plus a small
    reserve; it knows nothing about commands the app registers itself, and an app
    can easily have more of those than modules (cmdr-ipstube has 15). Shrinking to
    the computed value would silently drop them, so an existing larger setting is
    left alone — the registry warns at boot if it's genuinely too small."""
    n = _compute_max_commands(modules)
    existing = _read_max_commands(root)
    if existing is not None and existing > n:
        # Keep the larger value, but say so — on a RAM-tight target (AVR) an
        # inflated registry array costs bytes, and only you know whether the
        # headroom is still carrying app commands.
        print(f"  • MAX_COMMANDS={existing} (kept; computed {n} covers only cmdr's "
              f"modules, not commands your app registers — lower it by hand if you "
              f"want the RAM back)")
        return
    pio = root / "platformio.ini"
    cmake = root / "CMakeLists.txt"
    if pio.exists():
        text = re.sub(r"\n[ \t]*-DMAX_COMMANDS=\d+", "", pio.read_text())
        out = []
        for line in text.splitlines():
            out.append(line)
            if line.strip().startswith("build_flags"):
                out.append(f"    -DMAX_COMMANDS={n}")
        pio.write_text("\n".join(out) + "\n")
        print(f"  • MAX_COMMANDS={n}")
    elif cmake.exists():
        text = re.sub(r"add_compile_definitions\(MAX_COMMANDS=\d+\)\n", "", cmake.read_text())
        if "FetchContent_MakeAvailable(commander)" in text:
            text = text.replace("FetchContent_MakeAvailable(commander)",
                                f"add_compile_definitions(MAX_COMMANDS={n})\n"
                                "FetchContent_MakeAvailable(commander)", 1)
            cmake.write_text(text)
            print(f"  • MAX_COMMANDS={n}")


# Pico CMake injection for the controller module. CYW43_ENABLE_BLUETOOTH must
# precede pico_sdk_init() (it selects the BT firmware blob); COMMANDER_ENABLE_CONTROLLER
# must precede FetchContent_MakeAvailable so the runner builds the Bluepad32 target.
_CTRL_BT_DEF = "add_compile_definitions(CYW43_ENABLE_BLUETOOTH=1)  # commander controller (Bluetooth)"
_CTRL_OPT    = 'set(COMMANDER_ENABLE_CONTROLLER ON CACHE BOOL "" FORCE)  # commander controller'


def _controller_cmake_enable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        print("  ! no CMakeLists.txt — add CYW43_ENABLE_BLUETOOTH + COMMANDER_ENABLE_CONTROLLER manually")
        return
    text = cmake.read_text()
    changed = False
    if "CYW43_ENABLE_BLUETOOTH" not in text and "pico_sdk_init()" in text:
        text = text.replace("pico_sdk_init()", _CTRL_BT_DEF + "\npico_sdk_init()", 1)
        changed = True
    if "COMMANDER_ENABLE_CONTROLLER" not in text and "FetchContent_MakeAvailable(commander)" in text:
        text = text.replace("FetchContent_MakeAvailable(commander)",
                            _CTRL_OPT + "\n" + "FetchContent_MakeAvailable(commander)", 1)
        changed = True
    if changed:
        cmake.write_text(text)
        print("  • CMakeLists.txt: CYW43_ENABLE_BLUETOOTH=1 + COMMANDER_ENABLE_CONTROLLER=ON")
    print("  • needs BLUEPAD32_PATH (clone ricardoquesada/bluepad32); wipe build dir to reconfigure")


def _controller_cmake_disable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        return
    text = cmake.read_text()
    for line in (_CTRL_BT_DEF, _CTRL_OPT):
        text = text.replace(line + "\n", "")
    cmake.write_text(text)
    print("  • CMakeLists.txt: removed controller CMake lines (wipe build dir to reconfigure)")


# ESP32 CMake injection for the ipstube module. COMMANDER_ENABLE_IPSTUBE must be
# set before the IDF include so the runner component compiles IpstubeModule.cpp and
# pulls esp_lcd / esp_driver_spi / esp_driver_ledc into its REQUIRES.
_IPSTUBE_OPT = 'set(COMMANDER_ENABLE_IPSTUBE ON CACHE BOOL "" FORCE)  # commander ipstube (esp_lcd)'
_IPSTUBE_ANCHOR = "include($ENV{IDF_PATH}/tools/cmake/project.cmake)"


def _ipstube_cmake_enable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        print("  ! no CMakeLists.txt — set COMMANDER_ENABLE_IPSTUBE ON before the IDF include manually")
        return
    text = cmake.read_text()
    if "COMMANDER_ENABLE_IPSTUBE" not in text and _IPSTUBE_ANCHOR in text:
        text = text.replace(_IPSTUBE_ANCHOR, _IPSTUBE_OPT + "\n" + _IPSTUBE_ANCHOR, 1)
        cmake.write_text(text)
        print("  • CMakeLists.txt: COMMANDER_ENABLE_IPSTUBE=ON (runner builds esp_lcd driver)")
    print("  • six ST7789 on the IPSTube pinout; drive them from commander_on_ipstube_ready()")
    print("  • wipe the build dir (build-esp32/) to reconfigure")


def _ipstube_cmake_disable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        return
    text = cmake.read_text()
    text = text.replace(_IPSTUBE_OPT + "\n", "")
    cmake.write_text(text)
    print("  • CMakeLists.txt: removed COMMANDER_ENABLE_IPSTUBE (wipe build dir to reconfigure)")


# ESP32 CMake injection for the ws2812 module — same mechanism as ipstube: the
# COMMANDER_ENABLE_WS2812 cache var (set before the IDF include) makes the runner
# compile Ws2812Module.cpp (esp_driver_rmt is already required unconditionally).
_WS2812_OPT = 'set(COMMANDER_ENABLE_WS2812 ON CACHE BOOL "" FORCE)  # commander ws2812 (RMT)'


def _ws2812_cmake_enable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        print("  ! no CMakeLists.txt — set COMMANDER_ENABLE_WS2812 ON before the IDF include manually")
        return
    text = cmake.read_text()
    if "COMMANDER_ENABLE_WS2812" not in text and _IPSTUBE_ANCHOR in text:
        text = text.replace(_IPSTUBE_ANCHOR, _WS2812_OPT + "\n" + _IPSTUBE_ANCHOR, 1)
        cmake.write_text(text)
        print("  • CMakeLists.txt: COMMANDER_ENABLE_WS2812=ON (runner builds the RMT driver)")
    print("  • drive the LEDs from the shell (`wled`) or commander_on_ws2812_ready()")
    print("  • wipe the build dir (build-esp32/) to reconfigure")


def _ws2812_cmake_disable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        return
    text = cmake.read_text()
    text = text.replace(_WS2812_OPT + "\n", "")
    cmake.write_text(text)
    print("  • CMakeLists.txt: removed COMMANDER_ENABLE_WS2812 (wipe build dir to reconfigure)")


# ESP32 CMake injection for the aicam module's UART backend — same mechanism as
# ipstube/ws2812. Only needed for the uart transport (the i2c backend is portable
# header-only); COMMANDER_ENABLE_AICAM gates AiCamUartTransport.cpp in the runner.
_AICAM_OPT = 'set(COMMANDER_ENABLE_AICAM ON CACHE BOOL "" FORCE)  # commander aicam (Vision AI UART)'


def _aicam_cmake_enable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        print("  ! no CMakeLists.txt — set COMMANDER_ENABLE_AICAM ON before the IDF include manually")
        return
    text = cmake.read_text()
    if "COMMANDER_ENABLE_AICAM" not in text and _IPSTUBE_ANCHOR in text:
        text = text.replace(_IPSTUBE_ANCHOR, _AICAM_OPT + "\n" + _IPSTUBE_ANCHOR, 1)
        cmake.write_text(text)
        print("  • CMakeLists.txt: COMMANDER_ENABLE_AICAM=ON (runner builds the Vision AI UART backend)")
    print("  • Grove Vision AI V2 on UART1 (TX43/RX44); drive it via commander_on_aicam_ready()")
    print("  • wipe the build dir (build-esp32/) to reconfigure")


def _aicam_cmake_disable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        return
    text = cmake.read_text()
    text = text.replace(_AICAM_OPT + "\n", "")
    cmake.write_text(text)
    print("  • CMakeLists.txt: removed COMMANDER_ENABLE_AICAM (wipe build dir to reconfigure)")


# ESP32 CMake injection for the ir module — same mechanism as ws2812: the
# COMMANDER_ENABLE_IR cache var (set before the IDF include) makes the runner
# compile Esp32IRModule.cpp (esp_driver_rmt is already required unconditionally).
_IR_ESP32_OPT = 'set(COMMANDER_ENABLE_IR ON CACHE BOOL "" FORCE)  # commander ir (RMT)'


def _ir_esp32_cmake_enable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        print("  ! no CMakeLists.txt — set COMMANDER_ENABLE_IR ON before the IDF include manually")
        return
    text = cmake.read_text()
    if "COMMANDER_ENABLE_IR" not in text and _IPSTUBE_ANCHOR in text:
        text = text.replace(_IPSTUBE_ANCHOR, _IR_ESP32_OPT + "\n" + _IPSTUBE_ANCHOR, 1)
        cmake.write_text(text)
        print("  • CMakeLists.txt: COMMANDER_ENABLE_IR=ON (runner builds the RMT IR receiver)")
    print("  • connect a TSOP38238 (or similar) to the configured GPIO")
    print("  • wipe the build dir (build-esp32/) to reconfigure")


def _ir_esp32_cmake_disable() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        return
    text = cmake.read_text()
    text = text.replace(_IR_ESP32_OPT + "\n", "")
    cmake.write_text(text)
    print("  • CMakeLists.txt: removed COMMANDER_ENABLE_IR (wipe build dir to reconfigure)")


def cmd_module(args: argparse.Namespace) -> None:
    manifest = Path("cmdr.toml")

    if args.action == "list":
        target, modules, _autostart = read_manifest(manifest) if manifest.exists() else (detect_target(), {}, [])
        if not target:
            die("could not determine target — run from a commander project root (no cmdr.toml or platformio.ini)")
        print(f"target: {target}")
        width = max(len(n) for n in MODULE_SPECS)
        for name, spec in MODULE_SPECS.items():
            if spec["always"]:
                state = "always on"
            elif name in modules:
                opts = "  ".join(f"{k}={v}" for k, v in modules[name].items())
                state = f"ON   {opts}".rstrip()
            elif not _module_supported(name, target):
                if target == "unoq":
                    state = "n/a (Uno Q HAL: console + IR only)"
                elif target == "bluepill" and name in BLUEPILL_EXCLUDED:
                    state = "n/a (bluepill I2C HAL not implemented yet)"
                else:
                    state = f"n/a (needs {'/'.join(spec['platforms'])})"
            else:
                state = "off"
            print(f"  {name:{width}s} {state}")
        return

    name = args.name
    if name not in MODULE_SPECS:
        die(f"unknown module '{name}'. Available: {', '.join(MODULE_SPECS)}")
    spec = MODULE_SPECS[name]
    if spec["always"]:
        die(f"module '{name}' is always enabled")

    if manifest.exists():
        target, modules, autostart = read_manifest(manifest)
        target = target or detect_target()
    else:
        target = detect_target()
        modules = {}
        autostart = []
    if not target:
        die("could not determine target — run from a commander project root (no cmdr.toml or platformio.ini)")
    if not _module_supported(name, target):
        if target == "unoq":
            die(f"module '{name}' isn't available on the Uno Q yet — its Zephyr HAL backs only "
                f"the console/channel bus + IR so far (GPIO/I2C are stubbed).")
        if target == "bluepill" and name in BLUEPILL_EXCLUDED:
            die(f"module '{name}' isn't available on the Bluepill yet — its STM32 I2C HAL "
                f"is still stubbed (see PLAN.md \"Bluepill I2C\").")
        die(f"module '{name}' is not supported on target '{target}' (supports: {', '.join(spec['platforms'])})")

    # Some modules can't coexist: they own the same peripheral (roomba and
    # loco-bridge both drive Serial1) or register the same shell command (st7796
    # and st7789 both own `lcd`, and the second registration is silently shadowed
    # at dispatch). Declared per module as `conflicts` in MODULE_SPECS.
    if args.action == "enable":
        clash = set(spec.get("conflicts", [])) & modules.keys()
        if clash:
            other = next(iter(sorted(clash)))
            die(f"'{name}' conflicts with the enabled module '{other}' — they claim the same "
                f"command or peripheral. Disable it first: cmdr module disable {other}")

    if args.action == "enable":
        opts: dict = {}
        for key, prompt, default in spec["questions"]:
            if isinstance(default, dict):  # per-target default
                default = default.get(target, next(iter(default.values())))
            ans = input(f"{prompt} [{default}]: ").strip() or default
            try:
                opts[key] = int(ans, 0)
            except ValueError:
                opts[key] = ans
        for key, prompt, fdefault, _flag in spec.get("features", []):
            ans = input(f"{prompt} [{'Y/n' if fdefault else 'y/N'}]: ").strip().lower()
            opts[key] = (ans in ("y", "yes")) if ans else fdefault
        modules[name] = opts
        write_manifest(manifest, target, modules, autostart)   # preserve [autostart]
        _regenerate(target, modules, autostart)
        _install_tools(spec, target)   # serial tools, or unoq's channel-bus versions
        _add_pio_lib_deps(spec.get("pio_lib_deps", []))
        _sync_feature_flags(_feature_flags_on(modules))
        _sync_max_commands(modules)
        if name == "controller":
            _controller_cmake_enable()
        if name == "ipstube":
            _ipstube_cmake_enable()
        if name == "ws2812" and target == "esp32":
            _ws2812_cmake_enable()      # RMT backend needs a CMake opt-in
        elif name == "ws2812":
            # Pico: commander_pico_ws2812 is already linked into the runner.
            print("  • drive the LEDs from the shell (`wled`) or commander_on_ws2812_ready()")
        if name == "aicam" and modules[name].get("transport", "uart") != "i2c":
            _aicam_cmake_enable()
        if name == "ir" and target == "esp32":
            _ir_esp32_cmake_enable()
        print(f"enabled module: {name}")
    elif args.action == "disable":
        if name not in modules:
            print(f"module '{name}' is already disabled.")
            return
        del modules[name]
        write_manifest(manifest, target, modules, autostart)   # preserve [autostart]
        _regenerate(target, modules, autostart)
        _remove_tools(spec, target)
        _remove_pio_lib_deps(spec.get("pio_lib_deps", []))
        _sync_feature_flags(_feature_flags_on(modules))
        _sync_max_commands(modules)
        if name == "controller":
            _controller_cmake_disable()
        if name == "ipstube":
            _ipstube_cmake_disable()
        if name == "ws2812" and target == "esp32":
            _ws2812_cmake_disable()
        if name == "aicam":
            _aicam_cmake_disable()
        if name == "ir" and target == "esp32":
            _ir_esp32_cmake_disable()
        print(f"disabled module: {name}")


def cmd_autostart(args: argparse.Namespace) -> None:
    """Manage the boot commands dispatched at startup (cmdr.toml [autostart]). These run
    once after modules + transport are wired, for their side effect — e.g. `ir recv` to
    start the IR stream so a fresh board publishes presses with no command sent."""
    manifest = Path("cmdr.toml")
    if not manifest.exists():
        die("no cmdr.toml here — run from a commander project root")
    target, modules, autostart = read_manifest(manifest)
    target = target or detect_target()
    action = getattr(args, "action", None)

    if action in (None, "list"):
        if autostart:
            print("autostart (run at boot, in order):")
            for i, c in enumerate(autostart):
                print(f"  {i}: {c}")
        else:
            print('no autostart commands. Add one:  cmdr autostart add "ir recv"')
        return

    if action == "add":
        cmd = args.cmdline.strip()
        if not cmd:
            die("nothing to add")
        if cmd in autostart:
            print(f"already set: {cmd}")
            return
        autostart.append(cmd)
    elif action == "remove":
        cmd = args.cmdline.strip()
        if cmd not in autostart:
            die(f"not in autostart: {cmd}")
        autostart.remove(cmd)
    elif action == "clear":
        autostart = []

    write_manifest(manifest, target, modules, autostart)
    _regenerate(target, modules, autostart)
    print(f"autostart now: {autostart if autostart else '(none)'}")


# ── Scaffold functions ────────────────────────────────────────────────────────

def _emit_scripts(target: str, name: str, out_dir: Path, chip: str = "esp32s3",
                  dry: bool = False) -> "list[str]":
    """Write the per-target dev scripts + scripts/ helper copies. The single source of
    truth shared by `cmdr init` (scaffold_*) and `cmdr regen`, so the two can't drift.
    Does NOT write source, cmdr.toml, CMakeLists.txt/platformio.ini, or commander_modules.h
    — callers own those. Pico/pico2 emit nothing here (their dev scripts come from CMake's
    commander_generate_scripts at configure time). Returns the relative paths it writes;
    with dry=True it only collects them (no writes)."""
    written: list = []

    def _script(rel, content):
        if not dry:
            p = out_dir / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            write_script(p, content)
        written.append(rel)

    def _helper(rel, tmpl):
        if not dry:
            p = out_dir / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            copy_template(tmpl, p)
            p.chmod(0o755)
        written.append(rel)

    if target in ("uno", "r4"):
        _helper("scripts/find_port.py", "find_port.py")
        _helper("scripts/version_stamp.py", "version_stamp.py")
        if target == "uno":
            _helper("scripts/patch_freertos.py", "patch_freertos.py")
        _script("bum",     ARDUINO_BUM_SCRIPT)
        _script("build",   render(ARDUINO_BUILD_SCRIPT,  name=name))
        _script("upload",  render(ARDUINO_UPLOAD_SCRIPT, name=name, board_id=target))
        _script("monitor", render(ARDUINO_MONITOR_SCRIPT, board_id=target))
        if target == "r4":
            _script("bum-ota", render(ARDUINO_R4_BUM_OTA_SCRIPT, name=name))
    elif target == "bluepill":
        _helper("scripts/find_port.py", "find_port.py")
        _helper("scripts/stm32_build.py", "stm32_build.py")
        _script("bum",     ARDUINO_BUM_SCRIPT)
        _script("build",   render(BLUEPILL_BUILD_SCRIPT,  name=name))
        _script("upload",  render(BLUEPILL_UPLOAD_SCRIPT, name=name))
        _script("monitor", render(BLUEPILL_MONITOR_SCRIPT, name=name))
    elif target == "esp32":
        _helper("scripts/find_port.py", "find_port.py")
        # bake the active IDF export.sh (if any) so build/upload self-source it
        idf_path = os.environ.get("IDF_PATH", "")
        idf_export = f"{idf_path}/export.sh" if idf_path else ""
        _script("bum",     ESP32_BUM_SCRIPT)
        _script("build",   render(ESP32_BUILD_SCRIPT,  chip=chip, idf_export=idf_export))
        _script("upload",  render(ESP32_UPLOAD_SCRIPT, chip=chip, idf_export=idf_export))
        _script("monitor", render(ESP32_MONITOR_SCRIPT, chip=chip))
        # bum-ota is written by `cmdr enable ota`, but regen must be able to
        # reproduce it too — the dev scripts are gitignored, so a fresh clone of
        # an OTA-enabled project has no other way to get it back.
        _cmake = out_dir / "CMakeLists.txt"
        if _cmake.exists() and "COMMANDER_ENABLE_OTA" in _cmake.read_text():
            _script("bum-ota", render(ESP32_BUM_OTA_SCRIPT, name=name))
            _helper("scripts/ota_push.py", "ota_push.py")
    elif target == "unoq":
        _script("build",             UNOQ_BUILD_SCRIPT)
        _script("flash",             UNOQ_FLASH_SCRIPT)
        _script("monitor",           UNOQ_MONITOR_SCRIPT)
        _script("bum",               UNOQ_BUM_SCRIPT)
        _script("enable-flash-boot", UNOQ_ENABLE_FLASH_BOOT_SCRIPT)
        _script("install-broker",    UNOQ_INSTALL_BROKER_SCRIPT)
        _script("restore-arduino",   UNOQ_RESTORE_ARDUINO_SCRIPT)
        _script("deploy-sbc",        UNOQ_DEPLOY_SBC_SCRIPT)
    # pico/pico2: dev scripts come from CMake (commander_generate_scripts) → none here.
    return written


def scaffold_pico(target: str, name: str, out_dir: Path) -> None:
    board = PICO_TARGETS[target]
    ssid, password = wifi_credentials()
    (out_dir / "CMakeLists.txt").write_text(render(PICO_CMAKE_TEMPLATE, name=name, board=board))
    (out_dir / "main.cpp").write_text(render(HOOK_MAIN_CPP_TEMPLATE, name=name))
    (out_dir / "secrets.h").write_text(render(SECRETS_H_TEMPLATE, ssid=ssid, password=password))
    # Module manifest + generated registration (commander_modules.h sits next to
    # main.cpp at the project root for Pico).
    write_manifest(out_dir / "cmdr.toml", target, {})
    generate_modules_file(target, {}, out_dir / "commander_modules.h")
    _sync_max_commands({}, out_dir)
    copy_template("FreeRTOS_Kernel_import.cmake", out_dir / "FreeRTOS_Kernel_import.cmake")

    print(f"Created {out_dir}/ for {board}")
    if ssid == "your-network":
        print(f"Edit {out_dir}/secrets.h with your WiFi credentials\n")
    else:
        print(f"WiFi credentials pre-filled from ~/.cmdr/config\n")

    # Pre-flight: the configure step needs the Pico SDK + FreeRTOS kernel. A
    # missing env var would surface as a raw CMake include() failure, so check
    # first — the project is complete either way; configure finishes it later.
    missing = [v for v in ("PICO_SDK_PATH", "FREERTOS_KERNEL_PATH")
               if not os.environ.get(v)]
    if missing:
        print(f"Skipping cmake configure — {' and '.join(missing)} not set.")
        print("Install the SDKs (scripts/setup-sdks.sh in the commander repo) and export")
        print("the env vars (docs/getting-started.md), then configure the project:")
        print(f"  cd {out_dir} && cmake -B build-{target} -S . -DPICO_BOARD={board}")
        print("(the configure step also writes the ./bum build/upload/monitor scripts)")
        return

    subprocess.run(
        ["cmake", "-B", f"build-{target}", "-S", ".", f"-DPICO_BOARD={board}"],
        cwd=out_dir,
        check=True,
    )
    print(f"\nDone.\n  cd {out_dir}\n  ./bum")


def scaffold_arduino(target: str, name: str, out_dir: Path) -> None:
    is_r4 = target == "r4"

    pio_tmpl = ARDUINO_R4_PIO_TEMPLATE if is_r4 else ARDUINO_UNO_PIO_TEMPLATE
    (out_dir / "platformio.ini").write_text(render(pio_tmpl, name=name))

    src_dir = out_dir / "src"
    src_dir.mkdir()
    if is_r4:
        ssid, password = wifi_credentials()
        (out_dir / "secrets.h").write_text(render(SECRETS_H_TEMPLATE, ssid=ssid, password=password))
        (src_dir / "main.cpp").write_text(render(HOOK_MAIN_CPP_TEMPLATE, name=name))
        # Module manifest + generated registration (system enabled by default).
        write_manifest(out_dir / "cmdr.toml", "r4", {})
        generate_modules_file("r4", {}, src_dir / "commander_modules.h")
    else:
        (src_dir / "main.cpp").write_text(render(UNO_MAIN_CPP_TEMPLATE, name=name))
        write_manifest(out_dir / "cmdr.toml", "uno", {})
        generate_modules_file("uno", {}, src_dir / "commander_modules.h")

    _sync_max_commands({}, out_dir)
    _emit_scripts(target, name, out_dir)

    print(f"Created {out_dir}/ for Arduino {'R4 WiFi' if is_r4 else 'Uno'}")
    if is_r4:
        ssid, _ = wifi_credentials()
        if ssid == "your-network":
            print(f"Edit {out_dir}/secrets.h with your WiFi credentials")
        else:
            print("WiFi credentials pre-filled from ~/.cmdr/config")
    print(f"\nDone.\n  cd {out_dir}\n  ./bum")


UNOQ_README_TEMPLATE = """\
# __NAME__ — commander on the Arduino Uno Q

Two brains, tightly coupled: commander runs on the **STM32U585 (M33) via Zephyr**, and a
**Python broker on the QRB2210 Debian side** multiplexes the link. Your firmware is `src/main.cpp`
(`commander_config` + `commander_setup`); add stock features with `cmdr module enable <x>`.

`cmdr` only generates software — the steps that change the board are **scripts you run**, each
with a revert. Run them in this order on a fresh board:

## 1. Build your firmware (on the Mac) — also fetches the commander framework
```
./build      # west build (sets the gnuarmemb toolchain env — the Zephyr SDK has no Intel-Mac build).
             #   FetchContent pulls commander into build-unoq/, including the SBC broker.
./flash      # openocd-over-adb gdb load (west flash isn't working for this board upstream)
./monitor    # open the ch0 console over the USB-CDC gadget (tio); type `help`  (after ./install-broker)
./bum        # build + flash + monitor
```
Prereqs: a Zephyr/`west` workspace (commander's `scripts/setup-sdks.sh --zephyr` creates
`~/u-developer/zephyrproject`; a plain `~/zephyrproject` checkout also works), the Arm GNU
Toolchain, and `adb`. Override `ZEPHYRPROJECT` / `ZEPHYR_BASE` / `GNUARMEMB_TOOLCHAIN_PATH` /
`GDB` if yours live elsewhere.

## 2. One-time board setup (reversible — needs board sudo)
Run these **after a first `./build`** — `install-broker` pushes the broker from the fetched
commander source, so the framework has to be downloaded first:
```
./enable-flash-boot     # M33 boots your firmware from flash (sets STM32 option bytes).
                        #   Without it the chip boots its ROM bootloader and stays silent.
./install-broker        # broker service owns /dev/ttyHS1, bridges ch0 -> the Mac's USB
                        #   serial + fans chN -> /tmp/commander/chN.sock.
```

## 3. Modules
```
cmdr module enable ir    # NEC/Sony IR receive on D5 -> published on channel 1
cmdr module list
```
The Uno Q's menu is intentionally small — its Zephyr HAL backs the console/channel bus + IR so
far (GPIO/I2C sensor modules aren't offered until the HAL grows).

Enabling `ir` drops the **channel-bus IR tools** into `bin/` (`ir_map.py`, `ir_lookup.py`,
`ir_speak.py`, `irchan.py`) and seeds `maps/`. Unlike the serial boards, these run **on the SBC** —
they are **pure subscribers** to the broker's `ch1.sock` and start nothing. Receiving IR is a
standing board capability: turn it on once with autostart so a fresh board streams presses with
no command sent (and the human console stays private):
```
cmdr autostart add "ir recv"                             # one-time; board streams IR on boot
./deploy-sbc                                             # push bin/ tools + seed maps/ to the board
adb shell "cd /home/arduino && python3 ir_lookup.py"     # identify presses against maps/
adb shell "cd /home/arduino && python3 ir_speak.py"      # ...and speak the matched name (Piper TTS)
adb shell "cd /home/arduino && python3 ir_map.py -o sony.json"   # build a named map
adb pull /home/arduino/sony.json maps/                   # keep new maps under version control
```
(To inspect or stop the stream, open a command session on `ch2` — `ch2.sock` — and run `ir recv`
to toggle it; the consuming tools above never touch it.)
`ir_speak.py` speaks the matched button name through the warm Piper **TTS daemon** from
[unoq-tools](https://github.com/gbryant/unoq-tools) (install once: its `setup-tts.py`, then
`tts.py daemon install`); it falls back to `espeak-ng` if the daemon isn't running, or
prints-only if neither is available.
(Or just eyeball presses: `adb shell "socat - UNIX-CONNECT:/tmp/commander/ch1.sock"`.)

## Revert to stock Arduino
```
./restore-arduino    # stop commander's services, restore + start the router, optionally revert boot bytes
```

Background: `docs/zephyr-hal-spike.md` (boot fix + flash), `docs/commander-channels-bringup.md`
(the bus), `docs/unoq-access.md` (access map), `dev/unoq/` (the service units) — in the
commander repo.
"""


def scaffold_unoq(name: str, out_dir: Path) -> None:
    (out_dir / "CMakeLists.txt").write_text(render(UNOQ_CMAKE_TEMPLATE, name=name))
    (out_dir / "prj.conf").write_text(UNOQ_PRJCONF_TEMPLATE)
    (out_dir / "app.overlay").write_text(UNOQ_OVERLAY_TEMPLATE)
    (out_dir / "README.md").write_text(render(UNOQ_README_TEMPLATE, name=name))

    src_dir = out_dir / "src"
    src_dir.mkdir()
    (src_dir / "main.cpp").write_text(render(UNO_MAIN_CPP_TEMPLATE, name=name))  # hook main, no WiFi
    write_manifest(out_dir / "cmdr.toml", "unoq", {})
    generate_modules_file("unoq", {}, src_dir / "commander_modules.h")
    _sync_max_commands({}, out_dir)
    _emit_scripts("unoq", name, out_dir)

    print(f"Created {out_dir}/ for Arduino Uno Q (Zephyr M33 + channel bus + Linux broker)")
    print(f"\nNext (see README.md):\n  cd {out_dir}")
    print("  ./build               # builds + fetches the commander framework (incl. the broker)")
    print("One-time board setup, AFTER a first ./build (reversible, needs board sudo):")
    print("  ./enable-flash-boot   # M33 boots from flash (STM32 option bytes)")
    print("  ./install-broker      # broker service owns the link (pushed from the fetched source)")
    print("  ./bum                 # build + flash + monitor")


def scaffold_bluepill(name: str, out_dir: Path) -> None:
    (out_dir / "platformio.ini").write_text(render(BLUEPILL_PIO_TEMPLATE, name=name))

    src_dir = out_dir / "src"
    src_dir.mkdir()
    # Hook main (no WiFi); modules composed via the generated commander_modules.h.
    (src_dir / "main.cpp").write_text(render(UNO_MAIN_CPP_TEMPLATE, name=name))
    write_manifest(out_dir / "cmdr.toml", "bluepill", {})
    generate_modules_file("bluepill", {}, src_dir / "commander_modules.h")
    _sync_max_commands({}, out_dir)
    _emit_scripts("bluepill", name, out_dir)

    print(f"Created {out_dir}/ for STM32 Bluepill (USB-CDC console, ST-Link upload)")
    print("Build needs: $FREERTOS_KERNEL_PATH and a TinyUSB checkout")
    print("  ($TINYUSB_PATH, or $PICO_SDK_PATH/lib/tinyusb). Flash via ST-Link (SWD).")
    print(f"\nDone.\n  cd {out_dir}\n  ./bum")


def scaffold_esp32(name: str, out_dir: Path, chip: str, flash_mb: int, psram_mb: int) -> None:
    ssid, password = wifi_credentials()
    (out_dir / "CMakeLists.txt").write_text(render(ESP32_CMAKE_TEMPLATE, name=name))
    (out_dir / "sdkconfig.defaults").write_text(make_sdkconfig(chip, flash_mb, psram_mb))
    (out_dir / "secrets.h").write_text(render(SECRETS_H_TEMPLATE, ssid=ssid, password=password))

    main_dir = out_dir / "main"
    main_dir.mkdir()
    (main_dir / "CMakeLists.txt").write_text(ESP32_MAIN_CMAKE_TEMPLATE)
    (main_dir / "main.cpp").write_text(render(HOOK_MAIN_CPP_TEMPLATE, name=name))
    # Module manifest + generated registration (commander_modules.h sits next to
    # main.cpp in main/ for ESP-IDF).
    write_manifest(out_dir / "cmdr.toml", "esp32", {})
    generate_modules_file("esp32", {}, main_dir / "commander_modules.h")
    _sync_max_commands({}, out_dir)
    # _emit_scripts bakes the active IDF export.sh into build/upload (self-sourcing).
    _emit_scripts("esp32", name, out_dir, chip=chip)

    psram_str = f"{psram_mb} MB PSRAM" if psram_mb else "no PSRAM"
    print(f"Created {out_dir}/ [{chip}, {flash_mb} MB flash, {psram_str}]")
    print(f"Edit {out_dir}/secrets.h with your WiFi credentials")
    idf_export = f"{os.environ['IDF_PATH']}/export.sh" if os.environ.get("IDF_PATH") else ""
    if idf_export:
        print(f"(build/upload will source ESP-IDF from {idf_export} — override with $IDF_EXPORT)")
    else:
        print("(run `esp` once per shell, or set $IDF_EXPORT, so build/upload find ESP-IDF)")
    print(f"\nDone.\n  cd {out_dir}\n  ./bum")
    print(f"(First build runs 'idf.py set-target {chip}' automatically)")


# ── enable ota ────────────────────────────────────────────────────────────────

def _enable_ota_pico(cmake: Path, content: str) -> None:
    # 1. Insert COMMANDER_ENABLE_OTA flag before FetchContent_MakeAvailable
    content = content.replace(
        "FetchContent_MakeAvailable(commander)",
        "set(COMMANDER_ENABLE_OTA ON CACHE BOOL \"\" FORCE)\nFetchContent_MakeAvailable(commander)",
    )

    # 2. Insert PFB setup block after FetchContent_MakeAvailable
    content = content.replace(
        "FetchContent_MakeAvailable(commander)",
        f"FetchContent_MakeAvailable(commander)\n\n{PFB_BLOCK}",
    )

    # 3. Add pico_fota_bootloader_lib to target_link_libraries
    content = re.sub(
        r"(target_link_libraries\([^)]*commander::pico_runner)([^)]*\))",
        r"\1 pico_fota_bootloader_lib\2",
        content,
    )

    # 4. Append pfb_compile_with_bootloader after pico_add_extra_outputs
    m = re.search(r"add_executable\((\S+)", content)
    if not m:
        die("could not find add_executable in CMakeLists.txt")
    name = m.group(1)

    content = re.sub(
        rf"(pico_add_extra_outputs\({re.escape(name)}\))",
        rf"\1\npfb_compile_with_bootloader({name})",
        content,
    )

    cmake.write_text(content)
    print("Enabled OTA in CMakeLists.txt:")
    print("  • COMMANDER_ENABLE_OTA set before FetchContent_MakeAvailable")
    print("  • pico_fota_bootloader added (reads $PFB_PATH or ~/u-developer/pico_fota_bootloader)")
    print("  • pico_fota_bootloader_lib linked to target")
    print(f"  • pfb_compile_with_bootloader({name}) added")

    build_dirs = [d for d in Path(".").iterdir()
                  if d.is_dir() and (d / "CMakeCache.txt").exists()]
    if not build_dirs:
        print("\nNo build directory found — run cmake manually to configure.")
        return
    for build_dir in build_dirs:
        print(f"\nReconfiguring {build_dir}/...")
        subprocess.run(["cmake", "-B", str(build_dir)], check=True)


def _enable_ota_esp32(cmake: Path, content: str) -> None:
    # 1. Add COMMANDER_ENABLE_OTA before IDF include
    content = content.replace(
        "include($ENV{IDF_PATH}/tools/cmake/project.cmake)",
        "set(COMMANDER_ENABLE_OTA ON CACHE BOOL \"\" FORCE)\n"
        "include($ENV{IDF_PATH}/tools/cmake/project.cmake)",
    )
    cmake.write_text(content)

    # 2. Compose partitions.csv: add OTA, preserving any existing filesystem.
    flash_mb = _detect_flash_mb()
    parts = Path("partitions.csv")
    _, fs = parse_partitions(parts.read_text()) if parts.exists() else (False, [])
    parts.write_text(compose_partitions(flash_mb, ota=True, fs=fs))

    # 3. Add partition config to sdkconfig.defaults
    sdk_content = sdk.read_text() if sdk.exists() else ""
    if "CONFIG_PARTITION_TABLE_CUSTOM" not in sdk_content:
        sdk_content += (
            "\nCONFIG_PARTITION_TABLE_CUSTOM=y\n"
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"\n'
        )
        sdk.write_text(sdk_content)

    # 4. Delete sdkconfig so it regenerates with the new partition table
    for f in [Path("sdkconfig"), Path("build-esp32") / "sdkconfig"]:
        if f.exists():
            f.unlink()

    # 5. Detect project name and write bum-ota script
    m = re.search(r"project\((\S+)\)", content)
    name = m.group(1) if m else "app"
    write_script(Path("bum-ota"), render(ESP32_BUM_OTA_SCRIPT, name=name))
    copy_template("ota_push.py", Path("scripts") / "ota_push.py")

    print("Enabled OTA in CMakeLists.txt:")
    print("  • COMMANDER_ENABLE_OTA set")
    print(f"  • partitions.csv composed ({flash_mb} MB flash, dual OTA"
          + (", filesystem preserved" if fs else "") + ")")
    print("  • sdkconfig.defaults updated with custom partition table")
    print(f"  • bum-ota written (usage: ./bum-ota [host]  default: {name}.local)")

    build_dirs = [d for d in Path(".").iterdir()
                  if d.is_dir() and (d / "CMakeCache.txt").exists()]
    if not build_dirs:
        print("\nNo build directory found — run cmake manually to configure.")
        return
    for build_dir in build_dirs:
        print(f"\nReconfiguring {build_dir}/...")
        subprocess.run(["cmake", "-B", str(build_dir)], check=True)


# ── enable / disable littlefs (ESP32 filesystem partition) ────────────────────
# Adds a LittleFS data partition for runtime assets (fonts, images, config, logs),
# the joltwallet/esp_littlefs managed dependency, and a pre-build image of a source
# dir. Composes with OTA: the partition table reflows to fit whatever's enabled.

# esp_littlefs is not on the Espressif component registry (GitHub only), so it's
# pulled as a pinned git dependency — portable and offline-friendly, no registry.
_ESP_LITTLEFS_GIT = "https://github.com/joltwallet/esp_littlefs.git"
_ESP_LITTLEFS_VER = "v1.22.1"


def _add_git_dep(name: str, url: str, version: str) -> None:
    man = Path("main") / "idf_component.yml"
    block = f"  {name}:\n    git: {url}\n    version: \"{version}\"\n"
    if man.exists():
        text = man.read_text()
        if name in text:
            return
        if re.search(r"^dependencies:", text, re.MULTILINE):
            text = re.sub(r"(^dependencies:[^\n]*\n)", lambda m: m.group(1) + block,
                          text, count=1, flags=re.MULTILINE)
        else:
            text = text.rstrip() + "\ndependencies:\n" + block
        man.write_text(text)
    else:
        man.parent.mkdir(parents=True, exist_ok=True)
        man.write_text("dependencies:\n" + block)


def _reconfigure_cmake() -> None:
    dirs = _cmake_build_dirs()
    if not dirs:
        print("\nNo build directory found — run cmake manually to configure.")
        return
    for bd in dirs:
        print(f"\nReconfiguring {bd}/...")
        subprocess.run(["cmake", "-B", str(bd)], check=True)


def enable_littlefs(label: str = "storage", subdir: str = "storage",
                    size_mb: "int | None" = None) -> None:
    if Path("platformio.ini").exists():
        die("`cmdr enable littlefs` is esp32-only (CMake/ESP-IDF) for now")
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no CMakeLists.txt — run from your project root")
    content = cmake.read_text()
    if "IDF_PATH" not in content:        # esp32 marker (pico/unoq CMakeLists have no IDF_PATH)
        die("`cmdr enable littlefs` currently supports esp32 projects only")

    flash_mb = _detect_flash_mb()
    # 1. Compose partitions.csv: keep the OTA state, add/replace the FS partition.
    parts = Path("partitions.csv")
    ota, fs = parse_partitions(parts.read_text()) if parts.exists() else (False, [])
    fs = [f for f in fs if f[0] != label]                          # replace same-label FS
    fs.append((label, "littlefs", size_mb * 0x100000 if size_mb else None))
    parts.write_text(compose_partitions(flash_mb, ota=ota, fs=fs))

    # 2. sdkconfig.defaults → custom partition table (idempotent)
    sdk = Path("sdkconfig.defaults")
    sdk_content = sdk.read_text() if sdk.exists() else ""
    if "CONFIG_PARTITION_TABLE_CUSTOM" not in sdk_content:
        sdk.write_text(sdk_content + "\nCONFIG_PARTITION_TABLE_CUSTOM=y\n"
                       'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"\n')

    # 3. esp_littlefs as a pinned git dependency of the app
    _add_git_dep("esp_littlefs", _ESP_LITTLEFS_GIT, _ESP_LITTLEFS_VER)

    # 4. Pre-build image of the source dir (after project(), so the function exists)
    if "littlefs_create_partition_image" not in content:
        m = re.search(r"^project\([^\n]*\)\s*$", content, re.MULTILINE)
        if not m:
            die("could not find project() in CMakeLists.txt")
        block = (f"\n\n# LittleFS image ({subdir}/) flashed with the app — `cmdr enable littlefs`.\n"
                 f"littlefs_create_partition_image({label} ${{CMAKE_SOURCE_DIR}}/{subdir} FLASH_IN_PROJECT)")
        cmake.write_text(content[:m.end()] + block + content[m.end():])

    # 5. Source dir for filesystem contents
    d = Path(subdir)
    d.mkdir(parents=True, exist_ok=True)
    if not any(d.iterdir()):
        (d / ".gitkeep").write_text("")

    # 6. Regenerate sdkconfig against the new table
    for f in [Path("sdkconfig"), Path("build-esp32") / "sdkconfig"]:
        if f.exists():
            f.unlink()

    print(f"Enabled LittleFS '{label}' on {flash_mb} MB flash:")
    print(f"  • partitions.csv composed ({'dual OTA + ' if ota else ''}{label} filesystem"
          + (f", {size_mb} MB" if size_mb else ", remaining flash") + ")")
    print(f"  • esp_littlefs (git {_ESP_LITTLEFS_VER}) added to main/idf_component.yml")
    print(f"  • littlefs_create_partition_image() builds {subdir}/ into the flashed image")
    print(f"  • {subdir}/ created for filesystem contents")
    print("\nMount it in your app (the runner already pulls esp_littlefs via the dep):")
    print('  #include "commander_littlefs.h"')
    print(f'  commander_mount_littlefs("{label}", "/{label}");')
    _reconfigure_cmake()


def disable_littlefs(label: str = "storage") -> None:
    if Path("platformio.ini").exists():
        die("`cmdr enable littlefs` is esp32-only")
    parts = Path("partitions.csv")
    if not parts.exists():
        print("No partitions.csv — nothing to disable.")
        return
    ota, fs = parse_partitions(parts.read_text())
    if not any(f[0] == label for f in fs):
        print(f"No '{label}' filesystem partition found.")
        return
    fs = [f for f in fs if f[0] != label]
    if ota or fs:
        parts.write_text(compose_partitions(_detect_flash_mb(), ota=ota, fs=fs))
    else:
        parts.unlink()
    # Drop the image-build line for this label from CMakeLists.
    cmake = Path("CMakeLists.txt")
    if cmake.exists():
        text = cmake.read_text()
        text = re.sub(r"\n*# LittleFS image[^\n]*\nlittlefs_create_partition_image\("
                      + re.escape(label) + r"[^\n]*\n", "\n", text)
        cmake.write_text(text)
    for f in [Path("sdkconfig"), Path("build-esp32") / "sdkconfig"]:
        if f.exists():
            f.unlink()
    print(f"Disabled LittleFS '{label}'. Left main/idf_component.yml and {label}/ in place "
          "(remove by hand if unused).")
    _reconfigure_cmake()


def _env_name_from_pio(text: str) -> str:
    m = re.search(r"\[env:(.+?)\]", text)
    return m.group(1) if m else "app"


def _enable_ota_r4() -> None:
    pio = Path("platformio.ini")
    text = pio.read_text()
    name = _env_name_from_pio(text)
    if "COMMANDER_R4_OTA" in text:
        print("OTA already enabled.")
    else:
        out = []
        for line in text.splitlines():
            out.append(line)
            s = line.strip()
            if s.startswith("build_flags"):
                out.append("    -DCOMMANDER_R4_OTA")
            elif s.startswith("lib_deps"):
                out.append("    ArduinoOTA @ ^1.0.0")
        text = "\n".join(out) + "\n"
        # OTA needs the RAM — drop any FreeRTOS heap bump above the 8 KB default.
        text = re.sub(r"\n[^\n]*-DconfigTOTAL_HEAP_SIZE=[^\n]*", "", text)
        pio.write_text(text)
        print("Enabled OTA in platformio.ini:")
        print("  • -DCOMMANDER_R4_OTA build flag")
        print("  • ArduinoOTA @ ^1.0.0 lib_dep")
    write_script(Path("bum-ota"), render(ARDUINO_R4_BUM_OTA_SCRIPT, name=name))
    scripts = Path("scripts")
    scripts.mkdir(exist_ok=True)
    copy_template("upload_ota.py", scripts / "upload_ota.py")
    print("  • bum-ota + scripts/upload_ota.py written  (needs: pip install requests)")
    print("Flash once over USB, then: ./bum-ota")


def enable_ota() -> None:
    if Path("platformio.ini").exists():
        _enable_ota_r4()
        return
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no platformio.ini or CMakeLists.txt — run from your project root")
    content = cmake.read_text()
    if "COMMANDER_ENABLE_OTA" in content:
        print("OTA already enabled.")
        return
    # esp32 (ESP-IDF) vs pico/pico2 (Pico SDK). Both now FetchContent_MakeAvailable
    # commander, so discriminate on the IDF_PATH marker, not the FetchContent call.
    if "IDF_PATH" in content:
        _enable_ota_esp32(cmake, content)
    elif "FetchContent_MakeAvailable(commander)" in content:
        _enable_ota_pico(cmake, content)
    else:
        die("CMakeLists.txt does not reference commander — is this a commander project?")


# ── disable ota ───────────────────────────────────────────────────────────────

def _disable_ota_pico(cmake: Path, content: str) -> None:
    m = re.search(r"add_executable\((\S+)", content)
    if not m:
        die("could not find add_executable in CMakeLists.txt")
    name = m.group(1)

    # 1. Remove COMMANDER_ENABLE_OTA line
    content = content.replace(
        "set(COMMANDER_ENABLE_OTA ON CACHE BOOL \"\" FORCE)\n",
        "",
    )

    # 2. Remove PFB block
    content = content.replace(f"\n\n{PFB_BLOCK}", "")

    # 3. Remove pico_fota_bootloader_lib from target_link_libraries
    content = re.sub(
        r"(target_link_libraries\([^)]*commander::pico_runner) pico_fota_bootloader_lib([^)]*\))",
        r"\1\2",
        content,
    )

    # 4. Remove pfb_compile_with_bootloader line
    content = re.sub(
        rf"\npfb_compile_with_bootloader\({re.escape(name)}\)",
        "",
        content,
    )

    cmake.write_text(content)
    print("Disabled OTA in CMakeLists.txt:")
    print("  • COMMANDER_ENABLE_OTA removed")
    print("  • pico_fota_bootloader block removed")
    print("  • pico_fota_bootloader_lib unlinked")
    print(f"  • pfb_compile_with_bootloader({name}) removed")

    build_dirs = [d for d in Path(".").iterdir()
                  if d.is_dir() and (d / "CMakeCache.txt").exists()]
    if not build_dirs:
        print("\nNo build directory found — run cmake manually to configure.")
        return
    for build_dir in build_dirs:
        print(f"\nReconfiguring {build_dir}/...")
        subprocess.run(["cmake", "-B", str(build_dir), "-DCOMMANDER_ENABLE_OTA=OFF"], check=True)


def _disable_ota_esp32(cmake: Path, content: str) -> None:
    # 1. Remove COMMANDER_ENABLE_OTA line
    content = content.replace(
        "set(COMMANDER_ENABLE_OTA ON CACHE BOOL \"\" FORCE)\n",
        "",
    )
    cmake.write_text(content)

    # 2. Re-compose partitions.csv: drop OTA, but keep any filesystem partition.
    #    Only tear the custom table down entirely if nothing else needs it.
    p = Path("partitions.csv")
    _, fs = parse_partitions(p.read_text()) if p.exists() else (False, [])
    kept_fs = bool(fs)
    sdk = Path("sdkconfig.defaults")
    if kept_fs:
        p.write_text(compose_partitions(_detect_flash_mb(), ota=False, fs=fs))
    else:
        if p.exists():
            p.unlink()
        # 3. Remove partition config from sdkconfig.defaults (custom table no longer needed)
        if sdk.exists():
            sdk.write_text(sdk.read_text().replace(
                "\nCONFIG_PARTITION_TABLE_CUSTOM=y\n"
                'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"\n',
                "",
            ))

    # 4. Delete sdkconfig so it regenerates against the (changed) partition table
    for f in [Path("sdkconfig"), Path("build-esp32") / "sdkconfig"]:
        if f.exists():
            f.unlink()

    # 5. Remove bum-ota script
    bum_ota = Path("bum-ota")
    if bum_ota.exists():
        bum_ota.unlink()

    print("Disabled OTA in CMakeLists.txt:")
    print("  • COMMANDER_ENABLE_OTA removed")
    print("  • partitions.csv recomposed (single app, filesystem kept)" if kept_fs
          else "  • partitions.csv removed")
    if not kept_fs:
        print("  • sdkconfig.defaults partition config removed")
    print("  • bum-ota removed")

    build_dirs = [d for d in Path(".").iterdir()
                  if d.is_dir() and (d / "CMakeCache.txt").exists()]
    if not build_dirs:
        print("\nNo build directory found — run cmake manually to configure.")
        return
    for build_dir in build_dirs:
        print(f"\nReconfiguring {build_dir}/...")
        subprocess.run(["cmake", "-B", str(build_dir), "-DCOMMANDER_ENABLE_OTA=OFF"], check=True)


def _disable_ota_r4() -> None:
    pio = Path("platformio.ini")
    text = pio.read_text()
    if "COMMANDER_R4_OTA" not in text:
        print("OTA already disabled.")
    else:
        text = re.sub(r"\n[^\n]*-DCOMMANDER_R4_OTA", "", text)
        text = re.sub(r"\n[^\n]*ArduinoOTA @ \^1\.0\.0", "", text)
        pio.write_text(text)
        print("Disabled OTA in platformio.ini (removed flag + lib_dep)")
    for f in (Path("bum-ota"), Path("scripts/upload_ota.py")):
        if f.exists():
            f.unlink()
            print(f"  • removed {f}")


def disable_ota() -> None:
    if Path("platformio.ini").exists():
        _disable_ota_r4()
        return
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no platformio.ini or CMakeLists.txt — run from your project root")
    content = cmake.read_text()
    if "COMMANDER_ENABLE_OTA" not in content:
        print("OTA already disabled.")
        return
    # esp32 vs pico — discriminate on IDF_PATH (both use FetchContent_MakeAvailable now).
    if "IDF_PATH" in content:
        _disable_ota_esp32(cmake, content)
    elif "FetchContent_MakeAvailable(commander)" in content:
        _disable_ota_pico(cmake, content)
    else:
        die("CMakeLists.txt does not reference commander — is this a commander project?")


# ── enable / disable dfu (STM32 Bluepill) ─────────────────────────────────────
# Lets a Bluepill project choose ST-Link or USB-DFU upload. Enabling relocates the app
# above the davidgfnet DFU bootloader (0x08001000) and drops in the host scripts; ST-Link
# upload (./upload) still works once the bootloader is flashed.

_DFU_FILES = ["stm32f103c8_dfu.ld", "flash-bluepill-bootloader",
              "upload-bluepill-usb", "unlock-bluepill"]


def enable_dfu() -> None:
    pio = Path("platformio.ini")
    if not pio.exists() or "bluepill_f103c8" not in pio.read_text():
        die("`cmdr enable dfu` is only for the STM32 Bluepill target")
    text = pio.read_text()
    if "COMMANDER_STM32_DFU" in text:
        print("DFU already enabled.")
    else:
        out = []
        for line in text.splitlines():
            out.append(line)
            s = line.strip()
            if s.startswith("build_flags"):
                out.append("    -DCOMMANDER_STM32_DFU")
                # Keep the ELF's first LOAD segment at 0x08001000 (not rounded back to
                # the 64 KB boundary 0x08000000), so an ST-Link `program firmware.elf`
                # upload writes only the app region and never stamps the bootloader.
                out.append("    -Wl,-z,max-page-size=0x1000")
            elif s.startswith("board ") and "bluepill_f103c8" in s:
                out.append("board_build.ldscript = stm32f103c8_dfu.ld")
        pio.write_text("\n".join(out) + "\n")
        print("Enabled DFU in platformio.ini:")
        print("  • -DCOMMANDER_STM32_DFU (app @ 0x08001000 + the `bootloader` command)")
        print("  • board_build.ldscript = stm32f103c8_dfu.ld")
    for tmpl in _DFU_FILES:
        dest = Path(tmpl)
        copy_template(tmpl, dest)
        if not tmpl.endswith(".ld"):
            dest.chmod(0o755)
    print("  • " + ", ".join(_DFU_FILES))
    # `bum` now flashes over USB-DFU instead of ST-Link.
    if Path("bum").exists():
        write_script(Path("bum"), BLUEPILL_DFU_BUM_SCRIPT)
        print("  • bum now uploads over USB-DFU (./upload stays ST-Link)")
    print("\nFirst time — install the DFU bootloader via ST-Link (required once):")
    print("    ./flash-bluepill-bootloader")
    print("Then flash the app over USB — no ST-Link (the board is now in DFU):")
    print("    ./bum")
    print("Day-to-day just run ./bum (it auto-reboots the app into DFU, then flashes).")
    print("ST-Link app upload stays available anytime via ./upload.")


def disable_dfu() -> None:
    pio = Path("platformio.ini")
    if pio.exists():
        text = pio.read_text()
        if "COMMANDER_STM32_DFU" not in text:
            print("DFU already disabled.")
        else:
            text = re.sub(r"\n[ \t]*-DCOMMANDER_STM32_DFU", "", text)
            text = re.sub(r"\n[ \t]*-Wl,-z,max-page-size=0x1000", "", text)
            text = re.sub(r"\n[ \t]*board_build\.ldscript = stm32f103c8_dfu\.ld", "", text)
            pio.write_text(text)
            print("Disabled DFU in platformio.ini (app back to 0x08000000, ST-Link only).")
    for f in _DFU_FILES:
        p = Path(f)
        if p.exists():
            p.unlink()
            print(f"  • removed {f}")
    # Restore the standard build + ST-Link upload + monitor bum.
    if Path("bum").exists():
        write_script(Path("bum"), ARDUINO_BUM_SCRIPT)
        print("  • bum restored to ST-Link upload")


# ── CLI entry points ──────────────────────────────────────────────────────────

# Seeded into every new project so credentials and build output never land in
# git by accident (secrets.h holds WiFi credentials). Written once at init —
# the user owns it afterwards (regen never touches it).
PROJECT_GITIGNORE = """\
# credentials — never commit
secrets.h

# build output
build/
build-*/
.pio/
managed_components/
__pycache__/
# written by the build's version stamp so bum-ota can confirm an image landed
/.build_number

# cmdr-generated dev scripts — regenerate with `cmdr regen`, don't commit.
# They are generated, not source: a committed copy silently goes stale as the
# templates move on. On CMake targets (pico/pico2) the generator also bakes
# absolute paths and $BLUEPAD32_PATH into them, so they aren't portable anyway.
/bum
/build
/upload
/monitor
/bum-ota
/flash
# unoq board-management scripts (also cmdr-generated)
/enable-flash-boot
/install-broker
/restore-arduino
/deploy-sbc
"""


def cmd_init(args: argparse.Namespace) -> None:
    if not args.name.replace("-", "_").replace("_", "").isalnum():
        die(f"project name '{args.name}' contains invalid characters")

    out_dir = Path(args.name)
    if out_dir.exists():
        die(f"'{out_dir}' already exists")

    out_dir.mkdir(parents=True)
    (out_dir / ".gitignore").write_text(PROJECT_GITIGNORE)
    if args.target == "esp32":
        scaffold_esp32(args.name, out_dir, chip=args.chip, flash_mb=args.flash, psram_mb=args.psram)
    elif args.target in ARDUINO_TARGETS:
        scaffold_arduino(args.target, args.name, out_dir)
    elif args.target in STM32_TARGETS:
        scaffold_bluepill(args.name, out_dir)
    elif args.target in ZEPHYR_TARGETS:
        scaffold_unoq(args.name, out_dir)
    else:
        scaffold_pico(args.target, args.name, out_dir)


def cmd_enable(args: argparse.Namespace) -> None:
    if args.feature == "ota":
        enable_ota()
    elif args.feature == "dfu":
        enable_dfu()
    elif args.feature == "littlefs":
        enable_littlefs(label=args.label, subdir=args.dir, size_mb=args.size)


def cmd_disable(args: argparse.Namespace) -> None:
    if args.feature == "ota":
        disable_ota()
    elif args.feature == "dfu":
        disable_dfu()
    elif args.feature == "littlefs":
        disable_littlefs(label=args.label)


def cmd_update() -> None:
    subprocess.run([
        sys.executable, "-m", "pip", "install", "--force-reinstall",
        "git+https://github.com/gbryant/commander.git#subdirectory=tools/cmdr",
    ], check=True)


def cmd_pull() -> None:
    import shutil

    # ── PlatformIO project ────────────────────────────────────────────────────
    if Path("platformio.ini").exists():
        libdeps = Path(".pio") / "libdeps"
        removed_any = False
        if libdeps.is_dir():
            for env_dir in libdeps.iterdir():
                commander_dir = env_dir / "commander"
                if commander_dir.is_dir():
                    shutil.rmtree(commander_dir)
                    print(f"Removed {commander_dir}")
                    removed_any = True
        if not removed_any:
            print("No cached commander found in .pio/libdeps/ — nothing to remove.")
        print("Updating packages...")
        subprocess.run(["pio", "pkg", "update"], check=True)
        if Path("cmdr.toml").exists():       # keep MAX_COMMANDS sized to the modules
            _t, _mods, _as = read_manifest(Path("cmdr.toml"))
            _sync_max_commands(_mods)
        print("\nDone — commander updated.")
        return

    # ── CMake project (Pico / ESP32) ──────────────────────────────────────────
    build_dirs = [d for d in Path(".").iterdir()
                  if d.is_dir() and (d / "CMakeCache.txt").exists()]
    if not build_dirs:
        die("no build directory found — run cmake to configure first")

    for build_dir in build_dirs:
        for dep in ("commander-src", "commander-build", "commander-subbuild"):
            dep_path = build_dir / "_deps" / dep
            if dep_path.exists():
                shutil.rmtree(dep_path)
                print(f"Removed {dep_path}")
        print(f"Reconfiguring {build_dir}/...")
        subprocess.run(["cmake", "-B", str(build_dir)], check=True)
    print("\nDone — commander updated.")


def cmd_clean() -> None:
    """Remove build artifacts for a fresh build: CMake build dirs (build/, build-<target>/,
    incl. the FetchContent _deps under them), PlatformIO's .pio/, and esp32's generated
    sdkconfig. Source and cmdr-generated project files (main.cpp, cmdr.toml, scripts,
    CMakeLists.txt) are left alone. Because the per-build _deps go too, the next build
    re-fetches commander fresh — curing a stale *fetched* framework (but not stale project
    scripts, which are committed source; regenerate those separately)."""
    import shutil
    if not (Path("cmdr.toml").exists() or Path("platformio.ini").exists()
            or Path("CMakeLists.txt").exists()):
        die("not a commander project here (no cmdr.toml / platformio.ini / CMakeLists.txt) "
            "— refusing to clean")

    removed = []
    # CMake build dirs: by cmdr's naming convention (build/, build-<target>/) plus any
    # already-configured dir (has CMakeCache.txt), so unconfigured/failed dirs go too.
    cmake_dirs = {d for d in Path(".").iterdir()
                  if d.is_dir() and (d.name == "build" or d.name.startswith("build-"))}
    cmake_dirs |= set(_cmake_build_dirs())
    for d in sorted(cmake_dirs):
        shutil.rmtree(d, ignore_errors=True)
        removed.append(f"{d}/")

    pio = Path(".pio")                      # PlatformIO (uno / r4 / bluepill)
    if pio.is_dir():
        shutil.rmtree(pio, ignore_errors=True)
        removed.append(".pio/")

    sdk = Path("sdkconfig")                  # esp32: regenerated from sdkconfig.defaults
    if sdk.is_file() and Path("sdkconfig.defaults").exists():
        sdk.unlink()
        removed.append("sdkconfig")

    if removed:
        print("cleaned:")
        for r in removed:
            print(f"  {r}")
        print("next build reconfigures + re-fetches commander from scratch.")
    else:
        print("nothing to clean — no build artifacts found.")


def cmd_regen(args: argparse.Namespace) -> None:
    """Re-emit this project's cmdr-GENERATED files from the current templates — dev scripts,
    commander_modules.h (from cmdr.toml), and enabled modules' host tools — so a project
    adopts framework/tooling fixes without a re-init. Leaves hand-written source, cmdr.toml,
    and CMakeLists.txt / platformio.ini alone (those accumulate feature/version state and
    need targeted migrations, not regeneration). --dry-run lists changes without writing."""
    manifest = Path("cmdr.toml")
    if not manifest.exists():
        die("no cmdr.toml here — run from a commander project root")
    target, modules, autostart = read_manifest(manifest)
    target = target or detect_target()
    if not target:
        die("could not determine target from cmdr.toml")
    dry = getattr(args, "dry_run", False)

    # Recover the params init had as args: project name (pio env name, else dir name) and,
    # for esp32, the chip (from the existing build script's `set-target <chip>`).
    name = Path.cwd().name
    pio = Path("platformio.ini")
    if pio.exists():
        name = _env_name_from_pio(pio.read_text()) or name
    chip = "esp32s3"
    if target == "esp32" and Path("build").is_file():
        m = re.search(r"set-target\s+(\S+)", Path("build").read_text())
        if m:
            chip = m.group(1)

    written = _emit_scripts(target, name, Path("."), chip=chip, dry=dry)

    # commander_modules.h — regenerate from the manifest (+ autostart) in the board's layout.
    mod_rel = _modules_file_path(target)
    if not dry:
        generate_modules_file(target, modules, mod_rel, autostart)
    written.append(str(mod_rel))

    # Refresh enabled modules' host tools (e.g. the IR tools) — re-install from templates;
    # seeded data dirs (maps/) are preserved (existing files win).
    refreshed_tools = []
    for mname in modules:
        spec = MODULE_SPECS.get(mname, {})
        if spec.get("tools") or spec.get("unoq_tools"):
            if not dry:
                _install_tools(spec, target)
            refreshed_tools.append(mname)

    verb = "would regenerate" if dry else "regenerated"
    print(f"{verb}:")
    for w in written:
        print(f"  {w}")
    if refreshed_tools:
        print(f"  bin/ tools for: {', '.join(refreshed_tools)}")
    if target in ("pico", "pico2"):
        print("note: pico dev scripts come from CMake (commander_generate_scripts) — "
              "run `cmdr pull` or reconfigure to refresh them.")
    print("left untouched: your source, cmdr.toml, CMakeLists.txt / platformio.ini.")


def cmd_config(args: argparse.Namespace) -> None:
    cfg = load_config()
    if not cfg.has_section("wifi"):
        cfg.add_section("wifi")
    cfg.set("wifi", "ssid",     args.ssid)
    cfg.set("wifi", "password", args.password)
    save_config(cfg)
    print(f"Saved WiFi credentials to {CONFIG_PATH}")


# ── link / unlink (local commander source) ────────────────────────────────────
# Builds normally fetch commander from the pinned GitHub tag (FetchContent). For
# framework development you can point them at a local checkout instead, via CMake's
# FETCHCONTENT_SOURCE_DIR_COMMANDER. We persist that as a committed, machine-agnostic
# hook in CMakeLists.txt (an OPTIONAL include) plus a gitignored file holding the
# path — so the toggle survives clean builds and never lands in a teammate's tree.

_LOCAL_CMAKE = "commander_local.cmake"
_LOCAL_HOOK = (
    "# Local commander source override — managed by `cmdr link` / `cmdr unlink`.\n"
    "include(${CMAKE_SOURCE_DIR}/" + _LOCAL_CMAKE + " OPTIONAL)\n"
)


def _cmake_build_dirs() -> "list[Path]":
    return [d for d in Path(".").iterdir() if d.is_dir() and (d / "CMakeCache.txt").exists()]


def _reconfigure_commander(build_dirs: "list[Path]", unset: bool = False) -> None:
    import shutil
    for bd in build_dirs:
        for dep in ("commander-src", "commander-build", "commander-subbuild"):
            p = bd / "_deps" / dep
            if p.exists():
                shutil.rmtree(p)
        cmd = ["cmake", "-B", str(bd)]
        if unset:
            cmd += ["-U", "FETCHCONTENT_SOURCE_DIR_COMMANDER"]
        print(f"Reconfiguring {bd}/ ...")
        subprocess.run(cmd, check=True)


def _ensure_local_hook(cmake: Path) -> None:
    text = cmake.read_text()
    if _LOCAL_CMAKE in text:
        return                                   # hook already present
    for anchor in ("FetchContent_Populate(commander)", "FetchContent_MakeAvailable(commander)"):
        if anchor in text:
            cmake.write_text(text.replace(anchor, _LOCAL_HOOK + anchor, 1))
            return
    die("could not find a FetchContent(commander) call in CMakeLists.txt to anchor the hook")


def _gitignore_add(entry: str) -> None:
    gi = Path(".gitignore")
    lines = gi.read_text().splitlines() if gi.exists() else []
    if entry in lines:
        return
    with gi.open("a") as f:
        if lines and lines[-1].strip():
            f.write("\n")
        f.write(f"# Local commander source link (cmdr link)\n{entry}\n")


def cmd_link(args: argparse.Namespace) -> None:
    if Path("platformio.ini").exists():
        die("`cmdr link` is for CMake projects (pico/pico2/esp32/unoq). This is a\n"
            "PlatformIO project — point its lib_deps at a local path instead:\n"
            "  lib_deps = symlink://../commander")
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no CMakeLists.txt here — run from a cmdr project root")

    if args.path is None:                        # bare `cmdr link` → report status
        f = Path(_LOCAL_CMAKE)
        if f.exists():
            m = re.search(r'FETCHCONTENT_SOURCE_DIR_COMMANDER\s+"([^"]*)"', f.read_text())
            print(f"linked → {m.group(1) if m else '(unknown path)'}")
        else:
            print("not linked — building commander from the pinned GitHub source")
        return

    src = Path(args.path).expanduser().resolve()
    if not (src / "runners").is_dir() or not (src / "CMakeLists.txt").exists():
        die(f"{src} does not look like a commander checkout (no runners/ + CMakeLists.txt)")

    _ensure_local_hook(cmake)
    Path(_LOCAL_CMAKE).write_text(
        "# Generated by `cmdr link` — do not commit (machine-specific path).\n"
        f'set(FETCHCONTENT_SOURCE_DIR_COMMANDER "{src}" CACHE PATH "" FORCE)\n')
    _gitignore_add(_LOCAL_CMAKE)

    build_dirs = _cmake_build_dirs()
    if build_dirs:
        _reconfigure_commander(build_dirs)
    print(f"\nLinked commander → {src}")
    print("Builds now use this local checkout. Run `cmdr unlink` to revert to GitHub."
          + ("" if build_dirs else "\n(no build dir yet — applies on next configure)"))


def cmd_unlink(args: argparse.Namespace) -> None:
    if Path("platformio.ini").exists():
        die("`cmdr unlink` is for CMake projects (pico/pico2/esp32/unoq). This is a\n"
            "PlatformIO project — its commander version is the lib_deps git ref in\n"
            "platformio.ini; edit that directly.")
    f = Path(_LOCAL_CMAKE)
    if not f.exists():
        print("not linked — nothing to do")
        return
    f.unlink()
    print(f"Removed {_LOCAL_CMAKE}")
    build_dirs = _cmake_build_dirs()
    if build_dirs:
        _reconfigure_commander(build_dirs, unset=True)   # clear the sticky cache var + re-fetch
    print("\nUnlinked — builds fetch commander from GitHub."
          + ("" if build_dirs else " (no build dir to reconfigure)"))


# ── pin / unpin (commander version) ───────────────────────────────────────────
# Projects fetch commander with FetchContent at GIT_TAG. `main` floats (tracks the
# branch tip — not reproducible); pin to a fixed commit (or, later, a release tag)
# so the project always builds against a known-good framework. The pin lives in the
# committed CMakeLists.txt — it's the project's declared dependency version.

_PIN_RE = r"(FetchContent_Declare\(\s*commander\b.*?GIT_TAG\s+)(\S+)"


def _read_pin(text: str) -> "str | None":
    m = re.search(_PIN_RE, text, re.DOTALL)
    return m.group(2) if m else None


def _resolve_remote_main() -> str:
    r = subprocess.run(["git", "ls-remote", REPO_URL, "main"],
                       capture_output=True, text=True, check=True)
    sha = r.stdout.split()[0] if r.stdout.strip() else ""
    if not sha:
        die("could not resolve commander's main from the remote")
    return sha


def _apply_pin(cmake: Path, ref: str, cur: str) -> None:
    text = cmake.read_text()
    new, n = re.subn(_PIN_RE, lambda m: m.group(1) + ref, text, count=1, flags=re.DOTALL)
    if n == 0:
        die("could not find FetchContent_Declare(commander ... GIT_TAG ...) in CMakeLists.txt")
    cmake.write_text(new)
    print(f"Pinned commander → {ref}" + (f"  (was {cur})" if cur != ref else ""))
    bd = _cmake_build_dirs()
    if bd:
        _reconfigure_commander(bd)
    if Path(_LOCAL_CMAKE).exists():
        print("note: `cmdr link` is active — the pin is ignored until you `cmdr unlink`.")


def cmd_pin(args: argparse.Namespace) -> None:
    if Path("platformio.ini").exists():
        die("`cmdr pin` is for CMake projects (pico/pico2/esp32/unoq). This is a\n"
            "PlatformIO project — pin by appending a tag to the lib_deps git ref in\n"
            "platformio.ini, e.g. .../commander.git#v1.1")
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no CMakeLists.txt here — run from a cmdr project root")
    cur = _read_pin(cmake.read_text())
    if cur is None:
        die("no FetchContent_Declare(commander ...) GIT_TAG found in CMakeLists.txt")

    if not args.ref and not args.latest:                 # bare → status
        floating = cur in ("main", "master")
        print(f"commander pin: {cur}" + (" (floating — tracks the branch tip)"
                                         if floating else " (pinned)"))
        if Path(_LOCAL_CMAKE).exists():
            print("note: `cmdr link` is active — builds use a local checkout, not this pin.")
        return

    _apply_pin(cmake, _resolve_remote_main() if args.latest else args.ref, cur)


def cmd_unpin(args: argparse.Namespace) -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no CMakeLists.txt here — run from a cmdr project root")
    cur = _read_pin(cmake.read_text())
    if cur is None:
        die("no FetchContent_Declare(commander ...) GIT_TAG found in CMakeLists.txt")
    if cur in ("main", "master"):
        print(f"already floating on {cur}")
        return
    _apply_pin(cmake, "main", cur)


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="cmdr",
        description="Commander framework project manager.",
    )
    sub = parser.add_subparsers(dest="command", metavar="<command>")
    sub.required = True

    # ── init ──────────────────────────────────────────────────────────────────
    init_p = sub.add_parser("init", help="scaffold a new commander project")
    init_p.add_argument(
        "target",
        choices=list(TARGETS.keys()),
        help=f"hardware target ({', '.join(TARGETS)})",
    )
    init_p.add_argument("name", help="project name (becomes the directory and build target)")
    esp = init_p.add_argument_group("ESP32 options")
    esp.add_argument("--chip", default="esp32s3", metavar="CHIP",
                     help="IDF chip target (default: esp32s3)")
    esp.add_argument("--flash", type=int, default=16, choices=sorted(VALID_FLASH_MB), metavar="MB",
                     help="flash size in MB (default: 16)")
    esp.add_argument("--psram", type=int, default=8, choices=sorted(VALID_PSRAM_MB), metavar="MB",
                     help="PSRAM size in MB, 0 for none (default: 8)")

    # ── enable / disable ──────────────────────────────────────────────────────
    enable_p = sub.add_parser("enable", help="enable a feature in the current project")
    enable_p.add_argument("feature", choices=["ota", "dfu", "littlefs"], help="feature to enable")
    enable_p.add_argument("--size", type=int, metavar="MB",
                          help="littlefs: filesystem size in MB (default: remaining flash)")
    enable_p.add_argument("--label", default="storage",
                          help="littlefs: partition label / mount point (default: storage)")
    enable_p.add_argument("--dir", default="storage", dest="dir",
                          help="littlefs: source dir packed into the image (default: storage)")

    disable_p = sub.add_parser("disable", help="disable a feature in the current project")
    disable_p.add_argument("feature", choices=["ota", "dfu", "littlefs"], help="feature to disable")
    disable_p.add_argument("--label", default="storage",
                           help="littlefs: partition label to remove (default: storage)")

    # ── update / pull / clean ─────────────────────────────────────────────────
    sub.add_parser("update", help="update cmdr itself to latest")
    sub.add_parser("pull",   help="update commander library in current project and reconfigure")
    sub.add_parser("clean",  help="remove build artifacts (build dirs, .pio, fetched deps) for a fresh build")
    regen_p = sub.add_parser("regen", help="re-emit generated files (dev scripts, commander_modules.h, tools) from current templates")
    regen_p.add_argument("--dry-run", action="store_true", dest="dry_run",
                         help="list what would change without writing")

    # ── link / unlink ─────────────────────────────────────────────────────────
    link_p = sub.add_parser("link", help="build against a local commander checkout (bare: show status)")
    link_p.add_argument("path", nargs="?", help="path to a local commander checkout")
    sub.add_parser("unlink", help="revert to building commander from GitHub")

    # ── pin / unpin ───────────────────────────────────────────────────────────
    pin_p = sub.add_parser("pin", help="pin commander to a git ref (bare: show; --latest: freeze main)")
    pin_p.add_argument("ref", nargs="?", help="commit SHA, tag, or branch to pin")
    pin_p.add_argument("--latest", action="store_true",
                       help="resolve the remote main tip and pin that commit")
    sub.add_parser("unpin", help="float commander back to the main branch")

    # ── module ────────────────────────────────────────────────────────────────
    module_p = sub.add_parser("module", help="enable/disable/list modules in the current project")
    module_sub = module_p.add_subparsers(dest="action", metavar="<action>")
    module_sub.required = True
    me_p = module_sub.add_parser("enable", help="enable a module (asks its config questions)")
    me_p.add_argument("name", help=f"module to enable ({', '.join(MODULE_SPECS)})")
    md_p = module_sub.add_parser("disable", help="disable a module")
    md_p.add_argument("name", help="module to disable")
    module_sub.add_parser("list", help="list available and enabled modules")

    # ── autostart ───────────────────────────────────────────────────────────────
    autostart_p = sub.add_parser("autostart",
                                 help="manage commands run at boot (cmdr.toml [autostart])")
    as_sub = autostart_p.add_subparsers(dest="action", metavar="<action>")
    as_sub.add_parser("list", help="show autostart commands (default)")
    as_add = as_sub.add_parser("add", help="add a boot command")
    # NB: dest is "cmdline", not "command" — the top-level subparser already uses
    # dest="command", so a positional named "command" would clobber it.
    as_add.add_argument("cmdline", help='command line to run at boot, e.g. "ir recv"')
    as_rm = as_sub.add_parser("remove", help="remove a boot command")
    as_rm.add_argument("cmdline", help="the command line to remove")
    as_sub.add_parser("clear", help="remove all autostart commands")

    # ── config ────────────────────────────────────────────────────────────────
    config_p = sub.add_parser("config", help="set global cmdr preferences")
    config_sub = config_p.add_subparsers(dest="setting", metavar="<setting>")
    config_sub.required = True
    wifi_p = config_sub.add_parser("wifi", help="set default WiFi credentials")
    wifi_p.add_argument("ssid",     help="WiFi network name")
    wifi_p.add_argument("password", help="WiFi password")

    args = parser.parse_args()

    try:
        if args.command == "init":
            cmd_init(args)
        elif args.command == "enable":
            cmd_enable(args)
        elif args.command == "disable":
            cmd_disable(args)
        elif args.command == "update":
            cmd_update()
        elif args.command == "pull":
            cmd_pull()
        elif args.command == "clean":
            cmd_clean()
        elif args.command == "regen":
            cmd_regen(args)
        elif args.command == "link":
            cmd_link(args)
        elif args.command == "unlink":
            cmd_unlink(args)
        elif args.command == "pin":
            cmd_pin(args)
        elif args.command == "unpin":
            cmd_unpin(args)
        elif args.command == "module":
            cmd_module(args)
        elif args.command == "autostart":
            cmd_autostart(args)
        elif args.command == "config":
            cmd_config(args)
    except subprocess.CalledProcessError as exc:
        die(f"cmake step failed (exit {exc.returncode})")
    except Exception as exc:
        die(str(exc))


if __name__ == "__main__":
    main()
