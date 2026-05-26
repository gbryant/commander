"""commander-new — scaffold a new commander framework project."""

import argparse
import importlib.resources
import subprocess
import sys
from pathlib import Path

REPO_URL = "https://github.com/gbryant/commander.git"

TARGETS = {
    "pico":  "pico_w",
    "pico2": "pico2_w",
}

# ── Templates (use __NAME__ / __BOARD__ / __SHORT__ as placeholders) ──────────

CMAKE_TEMPLATE = """\
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


def render(template: str, name: str, board: str) -> str:
    return (
        template
        .replace("__NAME__", name)
        .replace("__BOARD__", board)
    )


def scaffold(target: str, name: str, out_dir: Path) -> None:
    board = TARGETS[target]

    out_dir.mkdir(parents=True, exist_ok=False)

    (out_dir / "CMakeLists.txt").write_text(render(CMAKE_TEMPLATE, name, board))
    (out_dir / "main.cpp").write_text(render(MAIN_CPP_TEMPLATE, name, board))
    (out_dir / "secrets.h").write_text(SECRETS_H_TEMPLATE)

    # Copy bundled FreeRTOS_Kernel_import.cmake into the project
    cmake_data = importlib.resources.files("commander_new.templates").joinpath(
        "FreeRTOS_Kernel_import.cmake"
    )
    (out_dir / "FreeRTOS_Kernel_import.cmake").write_bytes(cmake_data.read_bytes())

    print(f"Created {out_dir}/ for {board}")
    print(f"Edit {out_dir}/secrets.h with your WiFi credentials\n")

    subprocess.run(
        ["cmake", "-B", f"build-{target}", "-S", ".", f"-DPICO_BOARD={board}"],
        cwd=out_dir,
        check=True,
    )

    print(f"\nDone. Run ./{out_dir}/bum to build, upload, and monitor.")


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

    # Sanitise: project name should be a valid C identifier and directory name
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
        print(f"\nerror: build step failed (exit {exc.returncode})", file=sys.stderr)
        sys.exit(exc.returncode)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
