"""cmdr — Commander framework project manager."""

import argparse
import configparser
import importlib.resources
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

PICO_TARGETS = {
    "pico":  "pico_w",
    "pico2": "pico2_w",
}
ARDUINO_TARGETS = {"uno": "uno", "r4": "r4"}
STM32_TARGETS = {"bluepill": "bluepill_f103c8"}   # PlatformIO + native CMSIS (not Arduino)
TARGETS = {**PICO_TARGETS, "esp32": "esp32", **ARDUINO_TARGETS, **STM32_TARGETS}

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
    GIT_TAG        main
)
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

# Download commander source before IDF initializes (FetchContent_Populate
# does download only — does not process commander's CMakeLists.txt).
include(FetchContent)
FetchContent_Declare(commander
    GIT_REPOSITORY """ + REPO_URL + """
    GIT_TAG        main
)
FetchContent_Populate(commander)

set(COMMANDER_ROOT ${commander_SOURCE_DIR})
list(APPEND EXTRA_COMPONENT_DIRS ${commander_SOURCE_DIR}/runners/esp32)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(__NAME__)
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

ESP32_BUILD_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
build_flags =
    -DCOMMANDER_UNO_RUNNER
    -DMAX_COMMANDS=12
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
build_flags =
    -DCOMMANDER_R4_RUNNER
    -DMAX_COMMANDS=12
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
    PORT=$(python3 "$DIR/scripts/find_port.py" __BOARD_ID__ 2>/dev/null) && break
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
    PORT=$(python3 "$DIR/scripts/find_port.py" __CHIP__ 2>/dev/null) && break
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
    -DMAX_COMMANDS=12
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
    PORT=$(python3 "$DIR/scripts/find_port.py" bluepill 2>/dev/null) && break
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

def make_ota_partitions_csv(flash_mb: int) -> str:
    app_size = 0x1B0000 if flash_mb <= 4 else 0x300000
    app1_offset = 0x10000 + app_size
    return (
        f"# {flash_mb} MB flash — dual OTA\n"
        "# Name,   Type, SubType, Offset,   Size\n"
        f"nvs,      data, nvs,     0x9000,   0x5000\n"
        f"otadata,  data, ota,     0xe000,   0x2000\n"
        f"app0,     app,  ota_0,   0x10000,  {hex(app_size)}\n"
        f"app1,     app,  ota_1,   {hex(app1_offset)}, {hex(app_size)}\n"
    )


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
    "ir":      {"always": False, "platforms": ["pico", "pico2", "uno", "r4"], "questions": [
        ("gpio", "IR receive pin", {"pico": "22", "pico2": "22", "uno": "5", "r4": "5"}),
    ], "features": [
        ("wall", "Roomba virtual-wall detection?", False, "COMMANDER_IR_WALL"),
    ], "tools": ["irmap.py", "irlookup.py"], "seed_dirs": [("maps", "ir_maps")],
        "pio_lib_deps": ["IRremote"]},
    "roomba":  {"always": False, "platforms": ["r4"], "questions": [
        ("baud", "Roomba OI baud rate", "115200"),
        ("brc",  "BRC/wake pin (Mini-DIN 5), -1 if none", "-1"),
    ]},
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
    # R4 side: I2C-slave bridge that forwards CMD_LOCO_* to a Roomba over Serial1.
    # Self-contained — it also provides the `oi` debug command (via RoombaModule on
    # the shared driver), so it supersedes `roomba` on the R4 (mutually exclusive).
    "loco-bridge": {"always": False, "platforms": ["r4"], "questions": [
        ("addr", "This bridge's I2C slave address", "0x42"),
        ("port", "I2C port (0=Wire A4/A5 5V, 1=Wire1 Qwiic 3.3V)", "1"),
        ("baud", "Roomba OI baud rate", "115200"),
        ("brc",  "BRC/wake pin (Mini-DIN 5), -1 if none", "-1"),
    ]},
}


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
                 '#include "modules/locomotion/LocomotionBridge.h"'],
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
    die(f"no code emitter for module '{name}'")


def generate_modules_file(target: str, modules: dict, out_path: Path) -> None:
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

    # Modules that need tick() pumped (e.g. IR recv) get a strong, non-inline
    # commander_on_uart_ready() that overrides the runner's weak default and
    # registers them as UART tickers. Needs UartTransport's full definition.
    if tickers:
        includes.append('#include "transport/uart/UartTransport.h"')

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
        lines += [
            "",
            "extern \"C\" void commander_on_uart_ready(UartTransport &uart) {",
            *["    " + t for t in tickers],
            "}",
        ]
    out_path.write_text("\n".join(lines) + "\n")


# ── cmdr.toml manifest (minimal, dependency-free for our controlled schema) ───

def read_manifest(path: Path):
    target = None
    modules: dict = {}
    cur = None
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[module."):
            cur = modules.setdefault(line[len("[module."):].rstrip("]"), {})
            continue
        if line.startswith("["):
            cur = None
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
            if cur is None and k == "target":
                target = val
            elif cur is not None:
                cur[k] = val
    return target, modules


def write_manifest(path: Path, target: str, modules: dict) -> None:
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


def _regenerate(target: str, modules: dict) -> None:
    out = _modules_file_path(target)
    if not out.parent.exists():
        die(f"expected {out.parent}/ directory — run from your project root")
    generate_modules_file(target, modules, out)
    print(f"regenerated {out}")


# Companion host tooling a module ships (e.g. IR's irmap/irlookup). Tools go in
# bin/ (executable); tool_dirs are data dirs created empty (e.g. maps/).
def _install_tools(spec: dict) -> None:
    tools = spec.get("tools", [])
    if tools:
        bin_dir = Path("bin")
        bin_dir.mkdir(exist_ok=True)
        # Shared port detection so tools pick the right board by VID/PID (same
        # as the monitor script), not just the first cu.usb* device.
        copy_template("find_port.py", bin_dir / "find_port.py")
        (bin_dir / "find_port.py").chmod(0o755)
        for tool in tools:
            dest = bin_dir / tool
            copy_template(tool, dest)
            dest.chmod(0o755)
            print(f"  • bin/{tool}")
        print("    (the IR tools need: pip install pyserial)")
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


def _remove_tools(spec: dict) -> None:
    bin_dir = Path("bin")
    for tool in spec.get("tools", []):
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


def cmd_module(args: argparse.Namespace) -> None:
    manifest = Path("cmdr.toml")

    if args.action == "list":
        target, modules = read_manifest(manifest) if manifest.exists() else (detect_target(), {})
        print(f"target: {target or '(unknown)'}")
        for name, spec in MODULE_SPECS.items():
            if spec["always"]:
                state = "always on"
            elif name in modules:
                opts = "  ".join(f"{k}={v}" for k, v in modules[name].items())
                state = f"ON   {opts}".rstrip()
            elif spec["platforms"] is not None and target not in spec["platforms"]:
                state = f"n/a (needs {'/'.join(spec['platforms'])})"
            else:
                state = "off"
            print(f"  {name:10s} {state}")
        return

    name = args.name
    if name not in MODULE_SPECS:
        die(f"unknown module '{name}'. Available: {', '.join(MODULE_SPECS)}")
    spec = MODULE_SPECS[name]
    if spec["always"]:
        die(f"module '{name}' is always enabled")

    if manifest.exists():
        target, modules = read_manifest(manifest)
        target = target or detect_target()
    else:
        target = detect_target()
        modules = {}
    if not target:
        die("could not determine target — run from a commander project root (no cmdr.toml or platformio.ini)")
    if spec["platforms"] is not None and target not in spec["platforms"]:
        die(f"module '{name}' is not supported on target '{target}' (supports: {', '.join(spec['platforms'])})")

    # roomba and loco-bridge both own Serial1 (loco-bridge provides `oi` itself),
    # so only one may be enabled at a time.
    _SERIAL1_OWNERS = {"roomba", "loco-bridge"}
    if args.action == "enable" and name in _SERIAL1_OWNERS:
        other = (_SERIAL1_OWNERS - {name}) & modules.keys()
        if other:
            die(f"'{next(iter(other))}' already owns Serial1 — disable it before enabling '{name}' "
                f"(loco-bridge provides the `oi` command itself)")

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
        write_manifest(manifest, target, modules)
        _regenerate(target, modules)
        _install_tools(spec)
        _add_pio_lib_deps(spec.get("pio_lib_deps", []))
        _sync_feature_flags(_feature_flags_on(modules))
        if name == "controller":
            _controller_cmake_enable()
        print(f"enabled module: {name}")
    elif args.action == "disable":
        if name not in modules:
            print(f"module '{name}' is already disabled.")
            return
        del modules[name]
        write_manifest(manifest, target, modules)
        _regenerate(target, modules)
        _remove_tools(spec)
        _remove_pio_lib_deps(spec.get("pio_lib_deps", []))
        _sync_feature_flags(_feature_flags_on(modules))
        if name == "controller":
            _controller_cmake_disable()
        print(f"disabled module: {name}")


# ── Scaffold functions ────────────────────────────────────────────────────────

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
    copy_template("FreeRTOS_Kernel_import.cmake", out_dir / "FreeRTOS_Kernel_import.cmake")

    print(f"Created {out_dir}/ for {board}")
    if ssid == "your-network":
        print(f"Edit {out_dir}/secrets.h with your WiFi credentials\n")
    else:
        print(f"WiFi credentials pre-filled from ~/.cmdr/config\n")

    subprocess.run(
        ["cmake", "-B", f"build-{target}", "-S", ".", f"-DPICO_BOARD={board}"],
        cwd=out_dir,
        check=True,
    )
    print(f"\nDone.\n  cd {out_dir}\n  ./bum")


def scaffold_arduino(target: str, name: str, out_dir: Path) -> None:
    board_id = target  # "uno" or "r4"
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

    scripts_dir = out_dir / "scripts"
    scripts_dir.mkdir()
    copy_template("find_port.py", scripts_dir / "find_port.py")
    if not is_r4:
        copy_template("patch_freertos.py", scripts_dir / "patch_freertos.py")

    write_script(out_dir / "bum",     ARDUINO_BUM_SCRIPT)
    write_script(out_dir / "build",   render(ARDUINO_BUILD_SCRIPT,  name=name))
    write_script(out_dir / "upload",  render(ARDUINO_UPLOAD_SCRIPT, name=name, board_id=board_id))
    write_script(out_dir / "monitor", render(ARDUINO_MONITOR_SCRIPT, board_id=board_id))
    if is_r4:
        write_script(out_dir / "bum-ota", render(ARDUINO_R4_BUM_OTA_SCRIPT, name=name))

    print(f"Created {out_dir}/ for Arduino {'R4 WiFi' if is_r4 else 'Uno'}")
    if is_r4:
        ssid, _ = wifi_credentials()
        if ssid == "your-network":
            print(f"Edit {out_dir}/secrets.h with your WiFi credentials")
        else:
            print("WiFi credentials pre-filled from ~/.cmdr/config")
    print(f"\nDone.\n  cd {out_dir}\n  ./bum")


def scaffold_bluepill(name: str, out_dir: Path) -> None:
    (out_dir / "platformio.ini").write_text(render(BLUEPILL_PIO_TEMPLATE, name=name))

    src_dir = out_dir / "src"
    src_dir.mkdir()
    # Hook main (no WiFi); modules composed via the generated commander_modules.h.
    (src_dir / "main.cpp").write_text(render(UNO_MAIN_CPP_TEMPLATE, name=name))
    write_manifest(out_dir / "cmdr.toml", "bluepill", {})
    generate_modules_file("bluepill", {}, src_dir / "commander_modules.h")

    scripts_dir = out_dir / "scripts"
    scripts_dir.mkdir()
    copy_template("find_port.py", scripts_dir / "find_port.py")
    copy_template("stm32_build.py", scripts_dir / "stm32_build.py")

    write_script(out_dir / "bum",     ARDUINO_BUM_SCRIPT)
    write_script(out_dir / "build",   render(BLUEPILL_BUILD_SCRIPT,   name=name))
    write_script(out_dir / "upload",  render(BLUEPILL_UPLOAD_SCRIPT,  name=name))
    write_script(out_dir / "monitor", render(BLUEPILL_MONITOR_SCRIPT, name=name))

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

    scripts_dir = out_dir / "scripts"
    scripts_dir.mkdir()
    copy_template("find_port.py", scripts_dir / "find_port.py")

    write_script(out_dir / "bum",     ESP32_BUM_SCRIPT)
    write_script(out_dir / "build",   render(ESP32_BUILD_SCRIPT,   chip=chip))
    write_script(out_dir / "upload",  render(ESP32_UPLOAD_SCRIPT,  chip=chip))
    write_script(out_dir / "monitor", render(ESP32_MONITOR_SCRIPT, chip=chip))

    psram_str = f"{psram_mb} MB PSRAM" if psram_mb else "no PSRAM"
    print(f"Created {out_dir}/ [{chip}, {flash_mb} MB flash, {psram_str}]")
    print(f"Edit {out_dir}/secrets.h with your WiFi credentials")
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

    # 2. Detect flash size and write partitions.csv
    flash_mb = 16
    sdk = Path("sdkconfig.defaults")
    if sdk.exists():
        for line in sdk.read_text().splitlines():
            m = re.match(r"CONFIG_ESPTOOLPY_FLASHSIZE_(\d+)MB=y", line)
            if m:
                flash_mb = int(m.group(1))
                break
    app_size = 0x1B0000 if flash_mb <= 4 else 0x300000
    Path("partitions.csv").write_text(make_ota_partitions_csv(flash_mb))

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
    print(f"  • partitions.csv written ({flash_mb} MB flash, {hex(app_size)} per OTA slot)")
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
    if "FetchContent_MakeAvailable(commander)" in content:
        _enable_ota_pico(cmake, content)
    elif "FetchContent_Populate(commander)" in content:
        _enable_ota_esp32(cmake, content)
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

    # 2. Remove partitions.csv
    p = Path("partitions.csv")
    if p.exists():
        p.unlink()

    # 3. Remove partition config from sdkconfig.defaults
    sdk = Path("sdkconfig.defaults")
    if sdk.exists():
        sdk_content = sdk.read_text()
        sdk_content = sdk_content.replace(
            "\nCONFIG_PARTITION_TABLE_CUSTOM=y\n"
            'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"\n',
            "",
        )
        sdk.write_text(sdk_content)

    # 4. Delete sdkconfig so it regenerates without the partition table override
    for f in [Path("sdkconfig"), Path("build-esp32") / "sdkconfig"]:
        if f.exists():
            f.unlink()

    # 5. Remove bum-ota script
    bum_ota = Path("bum-ota")
    if bum_ota.exists():
        bum_ota.unlink()

    print("Disabled OTA in CMakeLists.txt:")
    print("  • COMMANDER_ENABLE_OTA removed")
    print("  • partitions.csv removed")
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
    if "FetchContent_MakeAvailable(commander)" in content:
        _disable_ota_pico(cmake, content)
    elif "FetchContent_Populate(commander)" in content:
        _disable_ota_esp32(cmake, content)
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

def cmd_init(args: argparse.Namespace) -> None:
    if not args.name.replace("-", "_").replace("_", "").isalnum():
        die(f"project name '{args.name}' contains invalid characters")

    out_dir = Path(args.name)
    if out_dir.exists():
        die(f"'{out_dir}' already exists")

    out_dir.mkdir(parents=True)
    if args.target == "esp32":
        scaffold_esp32(args.name, out_dir, chip=args.chip, flash_mb=args.flash, psram_mb=args.psram)
    elif args.target in ARDUINO_TARGETS:
        scaffold_arduino(args.target, args.name, out_dir)
    elif args.target in STM32_TARGETS:
        scaffold_bluepill(args.name, out_dir)
    else:
        scaffold_pico(args.target, args.name, out_dir)


def cmd_enable(args: argparse.Namespace) -> None:
    if args.feature == "ota":
        enable_ota()
    elif args.feature == "dfu":
        enable_dfu()


def cmd_disable(args: argparse.Namespace) -> None:
    if args.feature == "ota":
        disable_ota()
    elif args.feature == "dfu":
        disable_dfu()


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


def cmd_config(args: argparse.Namespace) -> None:
    cfg = load_config()
    if not cfg.has_section("wifi"):
        cfg.add_section("wifi")
    cfg.set("wifi", "ssid",     args.ssid)
    cfg.set("wifi", "password", args.password)
    save_config(cfg)
    print(f"Saved WiFi credentials to {CONFIG_PATH}")


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
    enable_p.add_argument("feature", choices=["ota", "dfu"], help="feature to enable")

    disable_p = sub.add_parser("disable", help="disable a feature in the current project")
    disable_p.add_argument("feature", choices=["ota", "dfu"], help="feature to disable")

    # ── update / pull ─────────────────────────────────────────────────────────
    sub.add_parser("update", help="update cmdr itself to latest")
    sub.add_parser("pull",   help="update commander library in current project and reconfigure")

    # ── module ────────────────────────────────────────────────────────────────
    module_p = sub.add_parser("module", help="enable/disable/list modules in the current project")
    module_sub = module_p.add_subparsers(dest="action", metavar="<action>")
    module_sub.required = True
    me_p = module_sub.add_parser("enable", help="enable a module (asks its config questions)")
    me_p.add_argument("name", help=f"module to enable ({', '.join(MODULE_SPECS)})")
    md_p = module_sub.add_parser("disable", help="disable a module")
    md_p.add_argument("name", help="module to disable")
    module_sub.add_parser("list", help="list available and enabled modules")

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
        elif args.command == "module":
            cmd_module(args)
        elif args.command == "config":
            cmd_config(args)
    except subprocess.CalledProcessError as exc:
        die(f"cmake step failed (exit {exc.returncode})")
    except Exception as exc:
        die(str(exc))


if __name__ == "__main__":
    main()
