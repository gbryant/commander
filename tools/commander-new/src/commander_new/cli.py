"""commander-new — scaffold a new commander framework project."""

import argparse
import importlib.resources
import os
import subprocess
import sys
from pathlib import Path

REPO_URL = "https://github.com/gbryant/commander.git"

PICO_TARGETS = {
    "pico":  "pico_w",
    "pico2": "pico2_w",
}
TARGETS = {**PICO_TARGETS, "esp32": "esp32s3"}

# ── Pico templates ────────────────────────────────────────────────────────────
# Placeholders: __NAME__, __BOARD__

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
#define WIFI_SSID     "your-network"
#define WIFI_PASSWORD "your-password"
"""

# ── ESP32 templates ───────────────────────────────────────────────────────────

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

ESP32_SDKCONFIG_DEFAULTS = """\
# ESP32-S3-N16R8: 16 MB flash, 8 MB OPI PSRAM, native USB console
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
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
    idf.py -B "$BUILD" set-target esp32s3
fi
idf.py -B "$BUILD" build
"""

ESP32_UPLOAD_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=$(python3 "$DIR/scripts/find_port.py" esp32s3)
echo "Flashing $PORT..."
idf.py -B "$DIR/build-esp32" -p "$PORT" flash
"""

ESP32_MONITOR_SCRIPT = """\
#!/bin/bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=""
for i in $(seq 1 20); do
    PORT=$(python3 "$DIR/scripts/find_port.py" esp32s3 2>/dev/null) && break
    [ $i -eq 1 ] && echo "Waiting for ESP32-S3..."
    sleep 0.5
done
[ -n "$PORT" ] || { echo "error: ESP32-S3 port did not appear" >&2; exit 1; }
echo "Connecting to $PORT  (Ctrl-T q to quit)"
tio --baudrate 115200 "$PORT"
"""


# ── Helpers ───────────────────────────────────────────────────────────────────

def render(template: str, name: str, board: str = "") -> str:
    return template.replace("__NAME__", name).replace("__BOARD__", board)


def write_script(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o755)


def copy_template(name: str, dest: Path) -> None:
    data = importlib.resources.files("commander_new.templates").joinpath(name)
    dest.write_bytes(data.read_bytes())


# ── Scaffold functions ────────────────────────────────────────────────────────

def scaffold_pico(target: str, name: str, out_dir: Path) -> None:
    board = PICO_TARGETS[target]

    (out_dir / "CMakeLists.txt").write_text(render(PICO_CMAKE_TEMPLATE, name, board))
    (out_dir / "main.cpp").write_text(render(MAIN_CPP_TEMPLATE, name))
    (out_dir / "secrets.h").write_text(SECRETS_H_TEMPLATE)
    copy_template("FreeRTOS_Kernel_import.cmake", out_dir / "FreeRTOS_Kernel_import.cmake")

    print(f"Created {out_dir}/ for {board}")
    print(f"Edit {out_dir}/secrets.h with your WiFi credentials\n")

    subprocess.run(
        ["cmake", "-B", f"build-{target}", "-S", ".", f"-DPICO_BOARD={board}"],
        cwd=out_dir,
        check=True,
    )

    print(f"\nDone. Run ./{out_dir}/bum to build, upload, and monitor.")


def scaffold_esp32(name: str, out_dir: Path) -> None:
    (out_dir / "CMakeLists.txt").write_text(render(ESP32_CMAKE_TEMPLATE, name))
    (out_dir / "sdkconfig.defaults").write_text(ESP32_SDKCONFIG_DEFAULTS)
    (out_dir / "secrets.h").write_text(SECRETS_H_TEMPLATE)

    main_dir = out_dir / "main"
    main_dir.mkdir()
    (main_dir / "CMakeLists.txt").write_text(ESP32_MAIN_CMAKE_TEMPLATE)
    (main_dir / "main.cpp").write_text(render(MAIN_CPP_TEMPLATE, name))

    scripts_dir = out_dir / "scripts"
    scripts_dir.mkdir()
    copy_template("find_port.py", scripts_dir / "find_port.py")

    write_script(out_dir / "bum",     ESP32_BUM_SCRIPT)
    write_script(out_dir / "build",   ESP32_BUILD_SCRIPT)
    write_script(out_dir / "upload",  ESP32_UPLOAD_SCRIPT)
    write_script(out_dir / "monitor", ESP32_MONITOR_SCRIPT)

    print(f"Created {out_dir}/ for esp32s3")
    print(f"Edit {out_dir}/secrets.h with your WiFi credentials")
    print(f"\nDone. Run ./{out_dir}/bum to build, upload, and monitor.")
    print(f"(First build runs 'idf.py set-target esp32s3' automatically)")


def scaffold(target: str, name: str, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=False)
    if target == "esp32":
        scaffold_esp32(name, out_dir)
    else:
        scaffold_pico(target, name, out_dir)


# ── CLI ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="commander-new",
        description="Scaffold a new commander framework project.",
    )
    parser.add_argument(
        "target",
        choices=list(TARGETS.keys()),
        help=f"Hardware target ({', '.join(TARGETS)})",
    )
    parser.add_argument("name", help="Project name (becomes the directory and CMake target)")

    args = parser.parse_args()

    if not args.name.replace("-", "_").replace("_", "").isalnum():
        print(f"error: project name '{args.name}' contains invalid characters", file=sys.stderr)
        sys.exit(1)

    out_dir = Path(args.name)
    if out_dir.exists():
        print(f"error: '{out_dir}' already exists", file=sys.stderr)
        sys.exit(1)

    try:
        scaffold(args.target, args.name, out_dir)
    except subprocess.CalledProcessError as exc:
        print(f"\nerror: cmake step failed (exit {exc.returncode})", file=sys.stderr)
        sys.exit(exc.returncode)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
