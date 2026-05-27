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
TARGETS = {**PICO_TARGETS, "esp32": "esp32", **ARDUINO_TARGETS}

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
    return {
        .wifi_ssid     = WIFI_SSID,
        .wifi_password = WIFI_PASSWORD,
        .hostname      = "__NAME__",
        .i2c_sda       = 4,
        .i2c_scl       = 5,
        .uart_baud     = 115200,
        .uart_greeting = "__NAME__",
    };
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
lib_deps =
    """ + REPO_URL + """
"""

ARDUINO_MAIN_CPP_TEMPLATE = """\
#include "commander.h"
#include "core/SystemModule.h"

static SystemModule sysModule;

extern "C" CommanderConfig commander_config() {
    return {
        .uart_baud     = 115200,
        .uart_greeting = "__NAME__",
    };
}

extern "C" void commander_setup(CommandRegistry& reg) {
    reg.registerModule(sysModule);
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
# Build __NAME__ and push via OTA (ArduinoOTA on the R4).
# Usage: ./bum-ota [host]   default: __NAME__.local
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${1:-__NAME__.local}"
"$DIR/build"
pio run -e __NAME__ -t upload --upload-port "$HOST"
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


# ── Scaffold functions ────────────────────────────────────────────────────────

def scaffold_pico(target: str, name: str, out_dir: Path) -> None:
    board = PICO_TARGETS[target]
    ssid, password = wifi_credentials()
    (out_dir / "CMakeLists.txt").write_text(render(PICO_CMAKE_TEMPLATE, name=name, board=board))
    (out_dir / "main.cpp").write_text(render(MAIN_CPP_TEMPLATE, name=name))
    (out_dir / "secrets.h").write_text(render(SECRETS_H_TEMPLATE, ssid=ssid, password=password))
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
        (src_dir / "main.cpp").write_text(render(MAIN_CPP_TEMPLATE, name=name))
    else:
        (src_dir / "main.cpp").write_text(render(ARDUINO_MAIN_CPP_TEMPLATE, name=name))

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


def scaffold_esp32(name: str, out_dir: Path, chip: str, flash_mb: int, psram_mb: int) -> None:
    ssid, password = wifi_credentials()
    (out_dir / "CMakeLists.txt").write_text(render(ESP32_CMAKE_TEMPLATE, name=name))
    (out_dir / "sdkconfig.defaults").write_text(make_sdkconfig(chip, flash_mb, psram_mb))
    (out_dir / "secrets.h").write_text(render(SECRETS_H_TEMPLATE, ssid=ssid, password=password))

    main_dir = out_dir / "main"
    main_dir.mkdir()
    (main_dir / "CMakeLists.txt").write_text(ESP32_MAIN_CMAKE_TEMPLATE)
    (main_dir / "main.cpp").write_text(render(MAIN_CPP_TEMPLATE, name=name))

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


def enable_ota() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no CMakeLists.txt in current directory — run from your project root")
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


def disable_ota() -> None:
    cmake = Path("CMakeLists.txt")
    if not cmake.exists():
        die("no CMakeLists.txt in current directory — run from your project root")
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
    else:
        scaffold_pico(args.target, args.name, out_dir)


def cmd_enable(args: argparse.Namespace) -> None:
    if args.feature == "ota":
        enable_ota()


def cmd_disable(args: argparse.Namespace) -> None:
    if args.feature == "ota":
        disable_ota()


def cmd_update() -> None:
    subprocess.run([
        sys.executable, "-m", "pip", "install", "--force-reinstall",
        "git+https://github.com/gbryant/commander.git#subdirectory=tools/cmdr",
    ], check=True)


def cmd_pull() -> None:
    import shutil
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
    enable_p.add_argument("feature", choices=["ota"], help="feature to enable")

    disable_p = sub.add_parser("disable", help="disable a feature in the current project")
    disable_p.add_argument("feature", choices=["ota"], help="feature to disable")

    # ── update / pull ─────────────────────────────────────────────────────────
    sub.add_parser("update", help="update cmdr itself to latest")
    sub.add_parser("pull",   help="update commander library in current project and reconfigure")

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
        elif args.command == "config":
            cmd_config(args)
    except subprocess.CalledProcessError as exc:
        die(f"cmake step failed (exit {exc.returncode})")
    except Exception as exc:
        die(str(exc))


if __name__ == "__main__":
    main()
