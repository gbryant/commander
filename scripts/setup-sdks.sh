#!/bin/bash
# setup-sdks.sh — bootstrap the external SDKs commander builds against.
#
# Each target board builds against vendor SDKs that are too large (and move too
# fast) to vendor into this repo, so the dev/<board>/ scripts expect them checked
# out on the host and located via env vars (with a default of ~/u-developer). This
# script clones them once and runs the ESP-IDF toolchain install.
#
# Idempotent — re-run it any time; existing checkouts are skipped, only missing
# ones are cloned. You do NOT need all of them: clone only what your target board
# uses (see the per-board matrix in docs/getting-started.md). Cloning the lot is
# fine and is what a full multi-board dev box wants.
#
# Install location: $COMMANDER_SDK_DIR (default ~/u-developer). The dev scripts
# default to the same path, so the common case needs zero env vars; set the per-SDK
# vars below only if you keep an SDK elsewhere.
set -e

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

# ESP32-S3 (ESP-IDF v5). The recursive clone is large; the toolchain install
# below downloads the cross-compiler + tools for esp32s3.
clone_or_skip https://github.com/espressif/esp-idf.git           "$SDK_DIR/esp-idf"         "--recursive"
if [ "$LAST_CLONED" -eq 1 ]; then
    echo "installing ESP-IDF toolchain for esp32s3..."
    "$SDK_DIR/esp-idf/install.sh" esp32s3
else
    echo "skip: ESP-IDF toolchain install already done"
fi

echo
echo "done. Add to your shell profile (~/.zshrc or ~/.bashrc):"
echo
echo "  export PICO_SDK_PATH=$SDK_DIR/pico-sdk"
echo "  export FREERTOS_KERNEL_PATH=$SDK_DIR/FreeRTOS-Kernel"
echo "  export BLUEPAD32_PATH=$SDK_DIR/bluepad32"
echo "  alias esp='. $SDK_DIR/esp-idf/export.sh'   # load the ESP-IDF build env"
echo
echo "(The esp32 dev scripts self-source $SDK_DIR/esp-idf/export.sh, so the alias"
echo " is only for running raw idf.py. STM32_DFU_BOOTLOADER_PATH / TINYUSB_PATH"
echo " default sensibly — see docs/getting-started.md for the full contract.)"
