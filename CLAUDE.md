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

### Pico W (next task — not yet working)
Build system is CMake + Pico SDK. See `platform/pico/CMakeLists.txt`.
Before building, the repo needs:
- `pico_sdk_import.cmake` — copy from `$PICO_SDK_PATH/external/`
- `FreeRTOS_Kernel_import.cmake` — copy from Pico SDK FreeRTOS port
- `platform/pico/FreeRTOSConfig.h` — exists in `micro-commander/`, copy over

```bash
cd platform/pico
cmake -B build -DPICO_BOARD=pico_w
cmake --build build
# flash: copy build/commander.uf2 to Pico mass storage
```

### ESP32 (scaffold only — not yet built)
Uses ESP-IDF. See `platform/esp32/CMakeLists.txt`.

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
│   └── uart/
│       ├── UartTransport.h      # line editor + dispatch; no FreeRTOS dep
│       └── UartTransport.cpp    # uses hal_uart_* only
└── platform/
    ├── arduino/
    │   ├── main.cpp
    │   ├── FreeRTOSConfig.h     # project-owned Uno config (overrides library)
    │   └── platformio.ini       # empty — use root platformio.ini
    ├── pico/
    │   ├── main.cpp
    │   └── CMakeLists.txt
    └── esp32/
        ├── main.cpp
        └── CMakeLists.txt
```

## What's working

- Arduino Uno: builds, uploads, `help` command works over serial.
- Pico W: builds clean, `help` confirmed over USB CDC serial.
- ESP32: scaffolded but not yet built or tested.

## What's next

Phase 1 — finish serial shell on all targets:
- Verify `platform/esp32` builds and `help` works over UART0

Phase 2 — sensor modules on Pico W:
- Prove `CompassModule` and `SonarModule` work unchanged on Pico W
- Add Pico-native IR module (PIO) implementing `IIRModule`
