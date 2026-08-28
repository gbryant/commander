#!/usr/bin/env bash
#
# commander's SWD implementation — the logic behind a project's ./flash, ./debug
# and ./reset scripts (written by `cmdr enable debug`).
#
# It lives in the framework rather than in each project so that fixes here reach
# every consumer through `cmdr pull`, instead of a copy going stale in each one.
# The project-side scripts are thin shims that locate this file and exec it;
# `install-broker` (dev/unoq/install_broker.sh) is the same pattern.
#
# What stays project-side is the *wiring description* — the generated
# openocd.cfg naming the probe interface, the target config and the adapter
# speed. This script knows how to drive openocd; it does not know what is on the
# other end of the cable.
#
#   swd.sh flash --cfg <openocd.cfg> --build-dir <dir> [--elf <path>]
#   swd.sh debug --cfg <openocd.cfg> --build-dir <dir> [--elf <path>] [--gdb <bin>]
#   swd.sh reset --cfg <openocd.cfg>
#
set -euo pipefail

ACTION="${1:-}"; shift || true

CFG=""; BUILD_DIR=""; ELF=""; GDB=""
while [ $# -gt 0 ]; do
    case "$1" in
        --cfg)       CFG="$2";       shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --elf)       ELF="$2";       shift 2 ;;
        --gdb)       GDB="$2";       shift 2 ;;
        *) echo "swd.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }

[ -n "$CFG" ] || die "no --cfg given"
[ -f "$CFG" ] || die "no $CFG — run \`cmdr enable debug\` to generate it"

command -v openocd >/dev/null 2>&1 || die \
"openocd not found on PATH.
  macOS:  brew install openocd
  Debian: sudo apt install openocd
It must be a build with CMSIS-DAP support (0.12 or newer covers RP2040/RP2350)."

# ── Locating the ELF ─────────────────────────────────────────────────────────
# openocd programs the ELF, not the .uf2 — it carries the symbols gdb needs and
# the load addresses openocd wants. Take an explicit --elf, else find the one in
# the build tree. Globbing rather than composing a name from the project keeps
# this working when the CMake target name and the directory name differ.
resolve_elf() {
    if [ -n "$ELF" ]; then
        [ -f "$ELF" ] || die "no $ELF — run ./build first"
        return
    fi
    [ -n "$BUILD_DIR" ] || die "no --elf and no --build-dir to search"
    [ -d "$BUILD_DIR" ] || die "no $BUILD_DIR/ — run ./build first"
    local found=()
    while IFS= read -r line; do found+=("$line"); done < <(
        find "$BUILD_DIR" -maxdepth 1 -name '*.elf' -type f 2>/dev/null | sort)
    case "${#found[@]}" in
        0) die "no .elf in $BUILD_DIR/ — run ./build first" ;;
        1) ELF="${found[0]}" ;;
        *) die "several .elf files in $BUILD_DIR/:
$(printf '  %s\n' "${found[@]}")
Pass the one you want: $0 $ACTION --cfg $CFG --elf <path>" ;;
    esac
}

case "$ACTION" in

flash)
    resolve_elf
    echo "flashing $ELF over SWD..."
    # `verify` reads the flash back: an SWD flash that silently half-wrote is
    # the failure this whole path exists to avoid. `reset` leaves the target
    # running rather than halted, so ./flash behaves like ./upload.
    openocd -f "$CFG" -c "program $ELF verify reset exit"
    echo "Done."
    ;;

reset)
    # No ELF needed — this is the one that has to work when the firmware is too
    # wedged to be told anything over the console.
    openocd -f "$CFG" -c "init; reset run; exit"
    ;;

debug)
    resolve_elf
    if [ -z "$GDB" ]; then
        for c in arm-none-eabi-gdb gdb-multiarch gdb; do
            if command -v "$c" >/dev/null 2>&1; then GDB="$c"; break; fi
        done
    fi
    [ -n "$GDB" ] || die \
"no ARM gdb found (looked for arm-none-eabi-gdb, gdb-multiarch, gdb).
  macOS:  brew install arm-none-eabi-gdb
  Debian: sudo apt install gdb-multiarch
Or pass one: ./debug --gdb <path>"

    LOG=$(mktemp -t commander-openocd.XXXXXX)
    openocd -f "$CFG" >"$LOG" 2>&1 &
    OCD=$!
    # Kill openocd when gdb exits, however it exits — otherwise it keeps the
    # probe claimed and the next ./flash fails with a confusing "unable to open".
    trap 'kill $OCD 2>/dev/null || true' EXIT
    for _ in $(seq 1 20); do
        grep -q "Listening on port 3333" "$LOG" 2>/dev/null && break
        kill -0 $OCD 2>/dev/null || break
        sleep 0.25
    done
    if ! kill -0 $OCD 2>/dev/null; then
        echo "openocd failed to start:" >&2
        cat "$LOG" >&2
        exit 1
    fi

    GDBARGS=(-ex "target extended-remote localhost:3333" -ex "set print pretty on")
    # commander's gdb helpers, if this framework checkout has them AND this gdb
    # can run them. Arm's own macOS toolchain builds ship WITHOUT Python
    # scripting, and gdb then reads the .py as a command file and spits
    # `Undefined command: ""` at startup — so check before offering it, and say
    # what to do instead of failing cryptically.
    HELPERS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/gdb/commander.py"
    if [ -f "$HELPERS" ]; then
        if "$GDB" --batch -ex "python pass" >/dev/null 2>&1; then
            GDBARGS=(-x "$HELPERS" "${GDBARGS[@]}")
        else
            echo "note: $GDB has no Python support, so commander's gdb helpers"
            echo "      (cmdr-commands / cmdr-tickers / cmdr-modules / cmdr-panic)"
            echo "      are unavailable. Arm's own toolchain builds omit Python;"
            echo "      brew install arm-none-eabi-gdb gets a build that has it."
        fi
    fi

    echo "openocd on :3333 (log: $LOG)"
    echo "  monitor reset init    reset and halt at the vector table"
    echo "  continue              run          ctrl-c   break in"
    "$GDB" "$ELF" "${GDBARGS[@]}"
    ;;

*)
    echo "usage: swd.sh <flash|debug|reset> --cfg <openocd.cfg> [--build-dir <dir>] [--elf <path>]" >&2
    exit 2
    ;;
esac
