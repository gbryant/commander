# commander — Claude context

## What this is

A portable embedded command shell targeting Arduino Uno (testbed), Raspberry Pi
Pico W, and ESP32. Same module code (compass, sonar, IR, etc.) runs on all
three. Platform-specific code is limited to `hal/`, `transport/`, and
`platform/`. See `PLAN.md` for roadmap and status.

## Architecture in one sentence

`core/` → `hal/<platform>/` → `modules/` → `transport/` → `platform/<board>/main.cpp`

Modules only include `hal/hal.h` and `core/`. The HAL is a C interface; one
`.cpp` per platform is compiled, the others are excluded by the build system.

## Key conventions

**No Arduino framework on Pico or ESP32.** Native Pico SDK and ESP-IDF only.
Arduino is used on the Uno testbed because of the Grove shield convenience.

**FreeRTOS everywhere.** Both Pico SDK and ESP-IDF ship FreeRTOS — same task
API across all targets. `UartTransport::taskBody` is a static method; platform
main calls `xTaskCreate`/`xTaskCreateStatic` and owns the stack size.

**HAL is a C interface** (`extern "C"`). I2C, GPIO, time, and UART. Platform
main calls `hal_i2c_init()` and `hal_uart_init()` before the scheduler starts.

**`i2c_ids.h` is the wire protocol spec.** Keep it identical across all
platforms. Do not add platform-specific IDs here.

**CMD macro gotcha.** The C preprocessor sees commas inside `{}` lambda bodies
as extra macro arguments. Always declare multiple variables on separate lines
inside `CMD(...)` handlers:
```cpp
// WRONG — breaks the CMD macro:
int16_t x, y, z;
// RIGHT:
int16_t x; int16_t y; int16_t z;
```

**`commander_on_panic()`** is a weak symbol in `core/CommandRegistry.cpp`.
Override it in platform main for board-specific diagnostics (LED blink, etc.).

## Building

### Arduino Uno
```bash
./bum-uno          # build + upload + monitor in one command
pio run -e uno     # build only
```
Port is hardcoded to `/dev/cu.usbmodem1413301` in `platformio.ini`.
`scripts/patch_freertos.py` runs pre-build to disable the FreeRTOS timer task
(saves ~480 bytes of heap) and reduce `configMINIMAL_STACK_SIZE` to 128.

### Arduino R4 WiFi
```bash
pio run -e r4                          # build only
pio run -e r4 -t upload                # upload via USB
pio device monitor -e r4              # serial monitor
```
FreeRTOS via `Arduino_FreeRTOS` library. WiFi + Telnet (port 23) live in the
runner; WiFi credentials from `secrets.h`. The `ArduinoTelnetTransport` in
`transport/telnet/arduino/` uses `WiFiServer` instead of lwIP sockets.

**OTA is opt-in** via `cmdr enable ota` (gated by `-DCOMMANDER_R4_OTA`, adds the
`ArduinoOTA` lib). It is **on-demand, not always-on**: WiFiS3's socket pool is
too small for OTA + Telnet to listen at once, so the `ota start` command closes
Telnet + mDNS and listens for an ArduinoOTA push on :65280 (reboots on success,
or resets after 60 s). The push runs from the single networking task (never a
separate task — modem race). `cmdr enable ota` writes a `bum-ota` script that
arms the device over Telnet, builds, and HTTP-POSTs via `scripts/upload_ota.py`.

### Pico W
Build system is CMake + Pico SDK. `pico_sdk_import.cmake` and
`FreeRTOS_Kernel_import.cmake` are checked in at the repo root.

```bash
./bum-pico           # build + upload + monitor in one command
./build-pico         # cmake build only
```

### Pico 2 W (RP2350)
Same CMakeLists as Pico W — board is overridden via `-DPICO_BOARD=pico2_w`.
Uses a separate build directory (`platform/pico/build-pico2/`).

```bash
./bum-pico2          # build + upload + monitor in one command
./build-pico2        # cmake build only
```
BOOTSEL volume is `/Volumes/RP2350` (vs `/Volumes/RPI-RP2` on RP2040).
`FreeRTOSConfig.h` auto-detects `PICO_RP2350` and enables dual-core SMP,
Cortex-M33 FPU, and 200 KB heap (vs 128 KB on RP2040).

### ESP32-S3-N16R8
Uses ESP-IDF v5. Project root is `platform/esp32/`; component sources are in `platform/esp32/main/`.

```bash
./bum-esp32          # build + upload + monitor in one command
./build-esp32        # build only (runs set-target on first run)
./upload-esp32       # flash via esptool (auto-detects port)
./monitor-esp32      # tio at 115200 baud
```

First build runs `idf.py set-target esp32s3` automatically (detects missing `sdkconfig`).
`sdkconfig.defaults` configures 16 MB flash and 8 MB OPI PSRAM for the N16R8 variant.
Transport uses native USB (USB Serial/JTAG, GPIO19/20) — connect tio to the `usbmodem` port.
The board has no USB-to-serial chip; UART0 (GPIO43/44) is on headers only.

**ESP-IDF environment:** run `esp` before any `idf.py` or `bum-esp32` command.
`esp` is a shell alias for `. ~/u-developer/esp-idf/export.sh`.

## File layout (key files)

```
commander/
├── PLAN.md                      # roadmap and status — update as work lands
├── CLAUDE.md                    # this file
├── platformio.ini               # Arduino Uno build (run from repo root)
├── bum-uno                      # build + upload + monitor script
├── scripts/patch_freertos.py    # pre-build FreeRTOS config patch for Uno
├── include/i2c_ids.h            # wire protocol — DO NOT diverge between platforms
├── core/                        # pure C++, zero platform deps
│   ├── IModule.h
│   ├── Writer.h
│   ├── CommandRegistry.h / .cpp
│   └── SystemModule.h           # provides `help` command
├── hal/
│   ├── hal.h                    # C interface: i2c, gpio, uart, time
│   ├── arduino/hal.cpp          # Wire + Arduino GPIO + Serial
│   ├── pico/hal.cpp             # Pico SDK
│   └── esp32/hal.cpp            # ESP-IDF v5 (i2c_master driver)
├── modules/                     # platform-independent sensor modules
│   ├── CompassModule.h          # HMC5883L via hal_i2c_*
│   ├── SonarModule.h            # PING-style sonar via hal_gpio_* + hal_pulse_in_us
│   └── ir/IIRModule.h           # abstract interface — implement per platform
├── transport/
│   ├── uart/
│   │   ├── UartTransport.h      # line editor + dispatch; no FreeRTOS dep
│   │   └── UartTransport.cpp    # uses hal_uart_* only
│   └── telnet/
│       ├── TelnetTransport.h    # lwIP BSD sockets — for Pico/ESP32
│       ├── TelnetTransport.cpp
│       └── arduino/
│           ├── ArduinoTelnetTransport.h   # WiFiServer — for Arduino WiFi platforms
│           └── ArduinoTelnetTransport.cpp
└── platform/
    ├── arduino/
    │   ├── main.cpp
    │   ├── FreeRTOSConfig.h     # project-owned Uno config (overrides library)
    │   └── platformio.ini       # empty — use root platformio.ini
    ├── arduino-r4/
    │   ├── main.cpp             # R4 WiFi: FreeRTOS + WiFi + OTA + Telnet
    │   └── platformio.ini       # empty — use root platformio.ini -e r4
    ├── pico/
    │   ├── main.cpp             # shared by Pico W (RP2040) and Pico 2 W (RP2350)
    │   ├── CMakeLists.txt       # PICO_BOARD=pico_w default; override with -DPICO_BOARD=pico2_w
    │   ├── FreeRTOSConfig.h     # auto-detects PICO_RP2350 for SMP + M33 config
    │   └── build/               # Pico W build dir
    │   └── build-pico2/         # Pico 2 W build dir (gitignored)
    └── esp32/
        ├── CMakeLists.txt
        ├── sdkconfig.defaults       # 16 MB flash, 8 MB OPI PSRAM, UART0 console
        └── main/
            ├── CMakeLists.txt       # idf_component_register with all sources
            └── main.cpp
```

## What's working

- Arduino Uno: builds, uploads, `help` command works over serial.
- Arduino R4 WiFi: hardware-confirmed — `help` + WiFi + Telnet + mDNS
  (`r4-test.local` resolves; telnet-by-name works) via `runners/arduino-r4`.
- Roomba OI: `modules/roomba/` (portable `Roomba` driver + `oi` shell command)
  drove a real Roomba from the R4 over `Serial1` (D0/D1). Hardware-confirmed.
- Pico W: builds clean, `help` confirmed over USB CDC serial.
- Pico 2 W (RP2350): builds clean via `./build-pico2`. Needs hardware test.
- ESP32-S3-N16R8: builds clean, `help` confirmed over native USB CDC (USB Serial/JTAG).

## What's next

Phase R0 — flash and confirm the two new platforms:
- ~~Flash Arduino R4 and confirm `help` + WiFi + Telnet~~ ✅ done (2026-05-29)
- Flash Pico 2 W and confirm `help` + WiFi

Phase R1 — Roomba driver module: ✅ done (2026-05-29)
- `modules/roomba/Roomba.h` (portable OI driver) + `RoombaModule.h` (`oi` command)
  drove a real Roomba from the R4 over `Serial1` (D0/D1).

Phase R2 — Pico 2 W as main controller (next):
- Define I2C bridge registers in `i2c_ids.h` (revisit `MOD_LOCOMOTION` fit)
- Arduino R4 becomes Roomba I2C bridge (I2C slave → Roomba OI)
