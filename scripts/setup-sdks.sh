#!/bin/bash
# setup-sdks.sh — bootstrap the external SDKs commander builds against.
#
# Each target board builds against vendor SDKs that are too large (and move too
# fast) to vendor into this repo, so the dev/<board>/ scripts expect them checked
# out on the host and located via env vars (with a default of ~/u-developer). This
# script clones them once and runs the heavier toolchain installs.
#
# Idempotent — re-run it any time; existing checkouts are skipped, only missing
# ones are cloned. You do NOT need all of them: clone only what your target board
# uses (see the per-board matrix in docs/getting-started.md). Cloning the lot is
# fine and is what a full multi-board dev box wants.
#
# The two HEAVY toolchains are opt-out / opt-in so you don't pay for a copy you
# won't use:
#   --no-esp-idf   skip ESP-IDF + its toolchain (default: install it)
#   --zephyr       ALSO set up the Zephyr workspace for the Uno Q M33 (default: off)
#
# Uno Q owners: this script covers the FIRMWARE side only. Board-side setup
# (first-boot provisioning wizard, Piper TTS, Bluetooth audio — all over adb)
# lives in the companion repo: https://github.com/gbryant/unoq-tools
#
# Install location: $COMMANDER_SDK_DIR (default ~/u-developer). The dev scripts
# default to the same path, so the common case needs zero env vars; set the per-SDK
# vars printed at the end only if you keep an SDK elsewhere.
set -e

DO_ESP=1
DO_ZEPHYR=0
for arg in "$@"; do
    case "$arg" in
        --no-esp-idf|--no-esp) DO_ESP=0 ;;
        --zephyr)              DO_ZEPHYR=1 ;;
        -h|--help)
            sed -n '/^# setup-sdks.sh/,/^set -e/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
    esac
done

SDK_DIR="${COMMANDER_SDK_DIR:-$HOME/u-developer}"
mkdir -p "$SDK_DIR"
echo "Installing SDKs under: $SDK_DIR"
echo

LAST_CLONED=0
clone_or_skip() {
    local url=$1 dest=$2 extra=${3:-}
    LAST_CLONED=0
    if [ -d "$dest/.git" ]; then
        echo "skip: $dest already exists"
    else
        echo "clone: $url"
        git clone $extra "$url" "$dest"
        LAST_CLONED=1
    fi
}

# ── Clone-model SDKs (small-to-medium; always installed) ──────────────────────
# Pico W / Pico 2 W (CMake) — and the Bluepill, which reuses FreeRTOS-Kernel.
clone_or_skip https://github.com/raspberrypi/pico-sdk.git        "$SDK_DIR/pico-sdk"        "--recursive"
clone_or_skip https://github.com/raspberrypi/FreeRTOS-Kernel.git "$SDK_DIR/FreeRTOS-Kernel"

# Pico pull-OTA bootloader (only needed for `cmdr enable ota` on Pico).
clone_or_skip https://github.com/JZimnol/pico_fota_bootloader.git "$SDK_DIR/pico_fota_bootloader"

# Bluetooth game-controller support (the Pico `controller` module).
clone_or_skip https://github.com/ricardoquesada/bluepad32.git    "$SDK_DIR/bluepad32"       "--recursive"

# STM32 Bluepill USB-DFU upload path (only needed for `cmdr enable dfu`).
clone_or_skip https://github.com/davidgfnet/stm32-dfu-bootloader.git "$SDK_DIR/stm32-dfu-bootloader"

# PNG decode for the ESP32 ipstube display image tooling.
clone_or_skip https://github.com/kikuchan/pngle.git              "$SDK_DIR/pngle"

# ── ESP-IDF (heavy: ~GB clone + its own toolchain) ────────────────────────────
if [ "$DO_ESP" -eq 1 ]; then
    clone_or_skip https://github.com/espressif/esp-idf.git       "$SDK_DIR/esp-idf"         "--recursive"
    if [ "$LAST_CLONED" -eq 1 ]; then
        echo "installing ESP-IDF toolchain for esp32s3..."
        "$SDK_DIR/esp-idf/install.sh" esp32s3
    else
        echo "skip: ESP-IDF toolchain install already done"
    fi
else
    echo "skip: ESP-IDF (--no-esp-idf)"
fi

# ── Zephyr workspace (heavy: west workspace; opt-in for the Uno Q M33) ─────────
# Reuses an existing ARM GNU toolchain (gnuarmemb) rather than installing the
# Zephyr SDK, so it adds no second compiler copy. This provisions the build env that
# `cmdr init unoq` projects need (they build via west / flash over on-board OpenOCD).
ARM_TC=""
detect_arm_toolchain() {
    if [ -n "${GNUARMEMB_TOOLCHAIN_PATH:-}" ] && [ -d "$GNUARMEMB_TOOLCHAIN_PATH" ]; then
        ARM_TC="$GNUARMEMB_TOOLCHAIN_PATH"; return
    fi
    # macOS Arm GNU Toolchain (pick the newest rel); gnuarmemb wants the dir whose
    # bin/ holds arm-none-eabi-gcc — matches the spike doc's path exactly.
    local cand
    cand=$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi 2>/dev/null | sort -V | tail -1)
    if [ -n "$cand" ]; then ARM_TC="$cand"; return; fi
    # Otherwise an arm-none-eabi-gcc on PATH (apt gcc-arm-none-eabi, Homebrew).
    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        ARM_TC="$(dirname "$(dirname "$(command -v arm-none-eabi-gcc)")")"   # parent of bin/
    fi
}

if [ "$DO_ZEPHYR" -eq 1 ]; then
    ZP="${ZEPHYRPROJECT:-$SDK_DIR/zephyrproject}"
    if [ -d "$ZP/.west" ]; then
        echo "skip: zephyr workspace already at $ZP"
    else
        echo "creating zephyr west workspace at $ZP ..."
        python3 -m venv "$ZP/.venv"
        "$ZP/.venv/bin/pip" install --upgrade pip west
        "$ZP/.venv/bin/west" init "$ZP"
        ( cd "$ZP" && "$ZP/.venv/bin/west" update )
        "$ZP/.venv/bin/west" zephyr-export
        "$ZP/.venv/bin/pip" install -r "$ZP/zephyr/scripts/requirements.txt"
    fi
    detect_arm_toolchain
    if [ -z "$ARM_TC" ]; then
        echo
        echo "WARNING: no ARM GNU toolchain found for Zephyr (gnuarmemb)." >&2
        echo "  Install one, then re-run or set GNUARMEMB_TOOLCHAIN_PATH:" >&2
        echo "    macOS:  brew install --cask gcc-arm-embedded" >&2
        echo "    Debian: apt install gcc-arm-none-eabi" >&2
    fi
fi

# ── Env-var contract ──────────────────────────────────────────────────────────
echo
echo "done. Add to your shell profile (~/.zshrc or ~/.bashrc):"
echo
echo "  export PICO_SDK_PATH=$SDK_DIR/pico-sdk"
echo "  export FREERTOS_KERNEL_PATH=$SDK_DIR/FreeRTOS-Kernel"
echo "  export BLUEPAD32_PATH=$SDK_DIR/bluepad32"
[ "$DO_ESP" -eq 1 ] && \
echo "  alias esp='. $SDK_DIR/esp-idf/export.sh'   # load the ESP-IDF build env"
if [ "$DO_ZEPHYR" -eq 1 ]; then
echo "  export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb"
echo "  export GNUARMEMB_TOOLCHAIN_PATH=${ARM_TC:-<path-to-arm-none-eabi>}"
echo "  # zephyr builds: source ${ZEPHYRPROJECT:-$SDK_DIR/zephyrproject}/.venv/bin/activate"
fi
echo
echo "(The esp32 dev scripts self-source $SDK_DIR/esp-idf/export.sh, so the alias"
echo " is only for running raw idf.py. STM32_DFU_BOOTLOADER_PATH / TINYUSB_PATH"
echo " default sensibly — see docs/getting-started.md for the full contract.)"
echo
echo "Uno Q owners: this covered the firmware side only — the board itself"
echo "(first-boot provisioning, Piper TTS, Bluetooth audio, all over adb) is"
echo "set up with the companion repo: https://github.com/gbryant/unoq-tools"
