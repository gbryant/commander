# Third-party code and dependencies

commander is [MIT licensed](LICENSE). This file records third-party code, split by
the distinction that matters legally: what is **in this repository** (and therefore
redistributed by it) versus what you **fetch yourself** to build.

## Vendored — code in this repository

| Path | Upstream | License |
|------|----------|---------|
| `third_party/stb/stb_truetype.h` | [nothings/stb](https://github.com/nothings/stb) v1.26, Sean Barrett / RAD Game Tools | Public domain **or** MIT, at your option (dual-licensed; see the LICENSE block at the end of the file) |
| `pico_sdk_import.cmake` | Copy of the shim from the Raspberry Pi Pico SDK | BSD-3-Clause, © 2020 Raspberry Pi (Trading) Ltd. |
| `FreeRTOS_Kernel_import.cmake` | Copy of the shim from the FreeRTOS kernel (`portable/ThirdParty/GCC/RP2040`) | BSD-3-Clause (`SPDX-License-Identifier: BSD-3-clause`) |

All three keep their original license headers intact — don't strip them when editing.

`stb_truetype.h` carries an explicit upstream warning worth repeating: it does no
range checking on font file offsets, so it should not be pointed at untrusted fonts.
commander uses it for text rendering on the ESP32 display path, where the fonts are
ones you put on the device yourself.

## External — you provide these; commander does not redistribute them

Builds locate these through environment variables or `FetchContent`, and they are
never copied into this tree. Their licenses govern your firmware, not this repo.

| Dependency | Used by | License |
|------------|---------|---------|
| [Pico SDK](https://github.com/raspberrypi/pico-sdk) (`PICO_SDK_PATH`) | Pico W, Pico 2 W | BSD-3-Clause |
| [ESP-IDF](https://github.com/espressif/esp-idf) | ESP32 targets | Apache-2.0 |
| [Zephyr](https://github.com/zephyrproject-rtos/zephyr) | Arduino Uno Q (STM32U585) | Apache-2.0 |
| FreeRTOS kernel | Pico, ESP32, Bluepill, Arduino | MIT |
| TinyUSB (`TINYUSB_PATH`, or the Pico SDK's copy) | STM32 Bluepill USB CDC | MIT |
| [Bluepad32](https://github.com/ricardoquesada/bluepad32) (`BLUEPAD32_PATH`) | `controller` module (Pico) | Apache-2.0 — **see the BTstack note below** |
| [IRremote](https://github.com/Arduino-IRremote/Arduino-IRremote) | `ir` module on Uno / R4 (via `lib_deps`) | MIT |
| [esp_littlefs](https://github.com/joltwallet/esp_littlefs) | `cmdr enable littlefs` (ESP32) | MIT |
| Arduino cores (AVR, Renesas RA / WiFiS3) | Uno, R4 | LGPL-2.1-or-later |

### BTstack (Bluepad32 / the `controller` module)

Bluepad32 is Apache-2.0, but it builds on **BlueKitchen's BTstack**, which is a
*commercial* license that is free for open-source projects. Bluepad32's own LICENSE
file leads with this notice. If you enable the `controller` module in a closed-source
or commercial product, check BlueKitchen's terms —
[bluekitchen-gmbh.com](http://bluekitchen-gmbh.com/). On Pico this reaches you through
the SDK's `pico_btstack`, whose Raspberry Pi terms cover use on their silicon.

This does not affect commander itself, or any project that doesn't enable
`controller`.

### STM32 DFU bootloader (GPL-3.0)

`dev/bluepill/flash-bootloader` can install the
[davidgfnet/stm32-dfu-bootloader](https://github.com/davidgfnet/stm32-dfu-bootloader),
which is **GPL-3.0**. To be explicit, because this is the one copyleft dependency in
the project:

- It is **not** in this repository. The script `git clone`s it to `$HOME` (overridable
  with `$STM32_DFU_BOOTLOADER_PATH`) at the moment you run it.
- It is a **separate program** flashed to its own region of the STM32's flash
  (0x08000000, first 4 KB). Your commander firmware is a distinct binary at
  0x08001000. They are not linked, and no GPL code is compiled into or linked against
  commander.
- It is entirely **opt-in** — only used if you choose the USB-DFU upload path instead
  of an ST-Link.

If you redistribute a board with that bootloader on it, the GPL-3.0 obligations apply
to the bootloader.
