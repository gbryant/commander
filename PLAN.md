# commander — project plan

## Goals

A portable embedded command shell that runs the same module code across multiple
target boards. Hardware modules (sensors, actuators) are developed and validated
on the Arduino Uno testbed, then deployed unchanged to Pico W and ESP32 targets
using their native SDKs — no Arduino core on production targets.

The framework is also a reusable library: external projects consume it via
CMake `FetchContent`, provide `commander_config()` / `commander_setup()`, and
get working firmware without touching WiFi, FreeRTOS, or panic-hook boilerplate.

## Principles

- No Arduino framework on Pico or ESP32. Use native SDKs (Pico SDK, ESP-IDF).
- FreeRTOS throughout — it ships with both Pico SDK and ESP-IDF, same task API.
- HAL isolates platform code. Modules depend only on `hal/hal.h` and `core/`.
- `i2c_ids.h` is the wire protocol. It must stay identical across all platforms.
- IR is platform-specific (IRremote / PIO / RMT) — implement `IIRModule` per target.

## Architecture

```
┌──────────────────────────────────────────────────────┐
│  External app  /  platform/pico/apps/commander/       │ ← commander_config()
│  commander_config() + commander_setup() + hooks       │   commander_setup()
├──────────────────────────────────────────────────────┤
│  runners/pico/runner.cpp  (commander::pico_runner)    │ ← main(), WiFi+mDNS,
│  runners/esp32/  (todo)                               │   FreeRTOS tasks,
│                                                        │   panic hooks
├──────────────────────────────────────────────────────┤
│  transport/uart/          transport/telnet/            │ ← how commands arrive
│  (commander::transport_uart/telnet)                   │   (serial / WiFi)
├──────────────────────────────────────────────────────┤
│  modules/                                             │ ← sensor & system modules
│  CompassModule  SonarModule  IIRModule  ...           │   zero platform code
├──────────────────────────────────────────────────────┤
│  hal/  arduino/hal.cpp  pico/hal.cpp  esp32/hal.cpp   │ ← I2C, GPIO, UART, time
│  (commander::hal_pico — one .cpp compiled per target) │
├──────────────────────────────────────────────────────┤
│  core/                                                │ ← pure C++, no platform
│  IModule  CommandRegistry  Writer  SystemModule       │   (commander::core)
├──────────────────────────────────────────────────────┤
│  include/i2c_ids.h                                    │ ← wire protocol spec
└──────────────────────────────────────────────────────┘
```

## External app usage (FetchContent)

```cmake
# Consumer's CMakeLists.txt — after pico_sdk_init()
include(FetchContent)
FetchContent_Declare(commander
    GIT_REPOSITORY https://github.com/gbryant/commander.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(commander)

add_executable(my_robot main.cpp)
target_link_libraries(my_robot PRIVATE commander::pico_runner)
commander_generate_scripts(my_robot)   # writes bum/build/upload/monitor/bum-ota
```

App provides two symbols + optional hooks (see `commander.h`):

```cpp
CommanderConfig commander_config();               // WiFi, I2C pins, baud, etc.
void            commander_setup(CommandRegistry&); // register modules
// optional weak overrides:
void commander_early_init();                       // pre-scheduler (BOOTSEL check)
void commander_on_uart_ready(UartTransport&);      // add tickers
void commander_on_wifi_connected();                // post-WiFi (launch PIO/core1)
```

## Status

| Area                              | State        | Notes                                               |
|-----------------------------------|--------------|-----------------------------------------------------|
| `core/` — registry, writer        | ✅ done      |                                                     |
| `include/i2c_ids.h`               | ✅ done      | wire protocol spec                                  |
| `hal/hal.h` interface             | ✅ done      | I2C, GPIO, time, UART                               |
| `hal/arduino/`                    | ✅ done      | Wire + Arduino GPIO + Serial                        |
| `hal/pico/`                       | ✅ done      | Pico SDK                                            |
| `hal/esp32/`                      | ✅ done      | ESP-IDF v5 i2c_master + UART driver                 |
| `modules/CompassModule`           | ✅ done      | HAL only                                            |
| `modules/SonarModule`             | ✅ done      |                                                     |
| `transport/uart/`                 | ✅ done      | platform-agnostic; begin() overload without baud    |
| `transport/telnet/`               | ✅ done      | lwIP BSD sockets (Pico/ESP32)                       |
| `transport/telnet/arduino/`       | ✅ done      | WiFiServer-based (R4)                               |
| `platform/arduino/`               | ✅ done      | Uno: builds clean; `help` confirmed over serial     |
| `platform/arduino-r4/`            | ✅ builds    | WiFi + OTA + Telnet; **needs hardware test**        |
| `platform/pico/` (Pico W)         | ✅ done      | `help` confirmed over USB CDC; WiFi + Telnet live   |
| `platform/pico/` (Pico 2W)        | ✅ done      | `help` + WiFi + Telnet confirmed; RP2350 INVPC fix  |
| `platform/esp32/`                 | ✅ done      | `help` confirmed over native USB CDC                |
| **CMake library targets**         | ✅ done      | `commander::core/hal_pico/transport_*/modules`      |
| **`runners/pico/`**               | ✅ done      | `commander::pico_runner`; owns main(), WiFi, hooks  |
| **`commander.h` API**             | ✅ done      | `CommanderConfig`, required + optional callbacks    |
| **FetchContent validation**       | ✅ done      | scratch project at `/tmp/commander-test-app` builds |
| **`commander_generate_scripts()`**| ✅ done      | generates bum/build/upload/monitor/bum-ota scripts  |
| `runners/esp32/`                  | ⬜ todo      | ESP-IDF component wrapper for runner pattern        |
| `modules/ir/IIRModule.h`          | ✅ done      | interface only                                      |
| `platform/arduino/IRModule`       | ⬜ todo      | IRremote                                            |
| `platform/pico/IRModule` (PIO)    | ✅ done      | PicoIRModule — ring buffer, NEC + Sony              |
| `platform/esp32/IRModule` (RMT)   | ⬜ todo      |                                                     |
| Roomba driver module              | ⬜ todo      | `modules/roomba/` — OI protocol, `hal_uart_*`       |
| Bluetooth module                  | ⬜ todo      |                                                     |

## Roadmap

### Phase L — library / runner pattern ✅ done (2026-05-26)

Commander is now consumable as a CMake FetchContent library.

- [x] Root `CMakeLists.txt` defines named targets: `commander::core`,
      `commander::hal_pico`, `commander::transport_uart`,
      `commander::transport_telnet`, `commander::modules`
- [x] `runners/pico/runner.cpp` owns `main()`, WiFi/mDNS init, FreeRTOS task
      wiring, watchdog panic hooks; `FreeRTOSConfig.h` + `lwipopts.h` live here
- [x] `commander.h` public API: `CommanderConfig`, `commander_config()`,
      `commander_setup()`, three optional weak hooks
- [x] `cmake/GenerateScripts.cmake`: `commander_generate_scripts(TARGET)` writes
      bum/build/upload/monitor/bum-ota scripts on cmake configure
- [x] `scripts/ota_push.py` extracted from inline Python in bum-ota scripts
- [x] FetchContent validated: scratch project builds `test_app.uf2` cleanly

### Phase R — robot integration

Goal: migrate Roomba robot to this framework.
- Pico 2 W = main controller (commander shell + FreeRTOS)
- Arduino R4 = Roomba OI bridge (I2C slave → Serial1 → Roomba)
- BT controller TBD

#### Phase R0 — platform proofs
- [x] `platform/arduino-r4/` builds (WiFi + OTA + Telnet + UART shell)
- [x] `platform/pico2/` builds and runs (RP2350 INVPC fault fixed 2026-05-25)
- [ ] **Flash R4 and confirm `help` + WiFi + Telnet** ← next hardware task

#### Phase R1 — Roomba driver module
- [ ] `modules/roomba/Roomba.h` — OI protocol driver using `hal_uart_*`
- [ ] Wire into `platform/arduino-r4/main.cpp` (OI on Serial1)
- [ ] `i2c_ids.h` — Roomba bridge command/sensor registers

#### Phase R2 — Pico 2 W as main controller
- [ ] R4 becomes I2C slave; Pico 2 W `RoombaModule` via `hal_i2c_*`
- [ ] Basic drive + stop from Pico shell

#### Phase R3 — Bluetooth controller
- [ ] Decide: Pico 2 W native BLE / dedicated Pico W / ESP32
- [ ] Controller input → locomotion commands

### What's next (step 4 candidates)

1. **ESP32 runner** — `runners/esp32/` using ESP-IDF component model; same
   `commander_config/setup` API. Completes multi-platform library story.
2. **R4 hardware test** — flash R4, confirm `help` + WiFi + Telnet; unblocks Phase R.
3. **Roomba module** — start Phase R1 (`modules/roomba/`); robot-focused.

## Board pin reference

| Signal    | Arduino Uno | Pico W (Grove) | Pico 2 W (Grove) | ESP32-S3-N16R8 |
|-----------|-------------|----------------|------------------|----------------|
| I2C SDA   | A4          | GP4            | GP4              | GPIO4          |
| I2C SCL   | A5          | GP5            | GP5              | GPIO5          |
| Sonar     | D4          | GP6            | GP6              | —              |
| IR recv   | D5          | GP22 (PIO)     | GP22 (PIO)       | — (RMT todo)   |
| UART TX   | —           | GP20 (stdio)   | GP20 (stdio)     | GPIO43         |
| UART RX   | —           | GP21 (stdio)   | GP21 (stdio)     | GPIO44         |
