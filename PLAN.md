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

- No Arduino framework on Pico, ESP32, or STM32. Use native SDKs (Pico SDK, ESP-IDF, CMSIS).
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
| `platform/arduino-r4/`            | ✅ done      | `help` + WiFi + Telnet + mDNS confirmed on hardware |
| `platform/pico/` (Pico W)         | ✅ done      | `help` confirmed over USB CDC; WiFi + Telnet live   |
| `platform/pico/` (Pico 2W)        | ✅ done      | `help` + WiFi + Telnet confirmed; RP2350 INVPC fix  |
| `platform/esp32/`                 | ✅ done      | `help` confirmed over native USB CDC                |
| `platform/stm32-bluepill/`        | ✅ done      | F103C8 native CMSIS+FreeRTOS+TinyUSB; blink/USART1/USB-CDC + USB-DFU confirmed |
| `hal/stm32/`                      | 🟡 partial   | CMSIS regs: GPIO/UART/time/USB done; **I2C stubbed** |
| `runners/stm32-bluepill/`         | ✅ done      | `COMMANDER_BLUEPILL_RUNNER` hook main for `cmdr init bluepill` |
| **CMake library targets**         | ✅ done      | `commander::core/hal_pico/transport_*/modules`      |
| **`runners/pico/`**               | ✅ done      | `commander::pico_runner`; owns main(), WiFi, hooks  |
| **`commander.h` API**             | ✅ done      | `CommanderConfig`, required + optional callbacks    |
| **FetchContent validation**       | ✅ done      | scratch project at `/tmp/commander-test-app` builds |
| **`commander_generate_scripts()`**| ✅ done      | generates bum/build/upload/monitor/bum-ota scripts  |
| **`runners/esp32/`**              | ✅ done      | IDF component; UART + WiFi + Telnet confirmed       |
| **`runners/arduino-r4/`**         | ✅ done      | FreeRTOS; UART + WiFi + Telnet + mDNS confirmed     |
| **`cmdr` tool**                   | ✅ done      | pip install; `init` uno/r4/pico/pico2/esp32/**bluepill**; `--chip/--flash/--psram`; `pull`/`update`/`config` |
| **`cmdr module` system**          | ✅ done      | enable/disable/list; cmdr.toml manifest; generates `commander_modules.h`; system/compass/sonar/ir/roomba |
| **`cmdr` features + tooling**     | ✅ done      | optional build-flag features (e.g. IR `wall`); per-module host tools in `bin/` + seed dirs; VID/PID port detection |
| OTA — R4 (on-demand)              | ✅ done      | `cmdr enable ota`; `ota start` hands off telnet→OTA; push confirmed on hardware (2026-05-29) |
| OTA — Pico (pull `ota <url>`)     | 🟡 untested  | runner wires `ota` + pfb_firmware_commit (COMMANDER_ENABLE_OTA); needs hardware test |
| OTA — ESP32 (pull `ota <url>`)    | 🟡 untested  | runner registers `ota` (COMMANDER_ENABLE_OTA); needs hardware test  |
| DFU upload — Bluepill             | ✅ done      | `cmdr enable dfu`; davidgfnet bootloader (`bootloader` cmd + dfu-util); HW-confirmed |
| Board commands — Pico             | ⬜ todo      | reboot-to-bootloader via SystemModule or hook       |
| `modules/ir/IIRModule.h`          | ✅ done      | interface only                                      |
| `platform/pico/IRModule` (PIO)    | ✅ done      | PicoIRModule (PIO+core1); in cmdr module system (`cmdr module enable ir`) |
| `platform/arduino/IRModule`       | ✅ done      | IRremote-based; in cmdr module system on Uno + R4 (`cmdr module enable ir`) |
| `platform/esp32/IRModule` (RMT)   | ⬜ todo      |                                                     |
| `modules/roomba/Roomba`           | ✅ done      | portable OI driver via abstract `RoombaPort`        |
| `modules/roomba/RoombaModule`     | ✅ done      | `oi` shell command; drove a real Roomba from R4     |
| Bluetooth module                  | ⬜ todo      |                                                     |

## Roadmap

### Phase L — library / runner pattern ✅ done (2026-05-26)

Commander is now consumable as a CMake FetchContent library.

- [x] Root `CMakeLists.txt` defines named targets: `commander::core`,
      `commander::hal_pico`, `commander::transport_uart`,
      `commander::transport_telnet`, `commander::modules`
- [x] `runners/pico/runner.cpp` owns `main()`, WiFi/mDNS init, FreeRTOS task
      wiring, watchdog panic hooks; `FreeRTOSConfig.h` + `lwipopts.h` live here
- [x] `runners/esp32/commander_runner/` — IDF component; same API; UART + WiFi +
      Telnet confirmed on XIAO ESP32-S3 (2026-05-26)
- [x] `commander.h` public API: `CommanderConfig`, `commander_config()`,
      `commander_setup()`, three optional weak hooks
- [x] `cmake/GenerateScripts.cmake`: `commander_generate_scripts(TARGET)` writes
      bum/build/upload/monitor/bum-ota scripts on cmake configure; no board suffix
- [x] `scripts/ota_push.py` extracted from inline Python in bum-ota scripts
- [x] FetchContent validated: scratch project builds `test_app.uf2` cleanly
- [x] `tools/cmdr` (formerly commander-new): pip-installable scaffolding tool; pico/pico2/esp32
      targets; `--chip/--flash/--psram` for ESP32 memory config

### Phase M — cmdr module system ✅ done (2026-05-31)

Modules are composed by `cmdr`, not by hand-editing `commander_setup()`.

- [x] `cmdr module enable/disable/list` → `cmdr.toml` manifest + generated
      `commander_modules.h` (in `src/`/root/`main/` by target); app `main.cpp`
      calls `commander_register_modules()`. Uno is in the system via a no-WiFi hook.
- [x] Modules: `system` (always), `compass`, `sonar` (cross-platform, HAL),
      `ir` (Pico PIO / Uno+R4 IRremote), `roomba` (R4). Per-target question
      defaults; `pio_lib_deps`; tick()-driven modules emit `commander_on_uart_ready`.
- [x] Optional `features` gated by a build flag (PlatformIO/CMake, ODR-consistent);
      first: IR `ir wall`, default off.
- [x] Companion host tooling installed to `bin/` (IR `irmap.py`/`irlookup.py`),
      seed dirs (IR `maps/` library), shared VID/PID port detection (`find_port.py`).
- [ ] Extend the module system + IR (RMT) to ESP32.

### Phase R — robot integration

Goal: migrate Roomba robot to this framework.
- Pico 2 W = main controller (commander shell + FreeRTOS)
- Arduino R4 = Roomba OI bridge (I2C slave → Serial1 → Roomba)
- BT controller TBD

#### Phase R0 — platform proofs ✅ done (2026-05-29)
- [x] `platform/arduino-r4/` builds (WiFi + OTA + Telnet + UART shell)
- [x] `platform/pico2/` builds and runs (RP2350 INVPC fault fixed 2026-05-25)
- [x] **Flash R4 and confirm `help` + WiFi + Telnet** — done via `runners/arduino-r4`;
      also `mDNS` (`r4-test.local` resolves; telnet-by-name confirmed) (2026-05-29)

#### Phase R1 — Roomba driver module — driver + shell done (2026-05-29)
- [x] `modules/roomba/Roomba.h` — portable OI driver via abstract `RoombaPort`
      (byte I/O + timing + optional BRC); no Arduino/HAL deps
- [x] `modules/roomba/RoombaModule.h` — `oi` shell command (drive/clean/dock/sensors)
- [x] R4 `Serial1` (D0/D1) adapter; **drove a real Roomba from the console** (2026-05-29)
- [x] `i2c_ids.h` — `MOD_LOCOMOTION` confirmed as the right fit; bridge command +
      sensor registers landed in Phase R2 (`CMD_LOCO_DRIVE/STOP/SENSORS`)

#### Phase R2 — Pico 2 W as main controller — DONE, hardware-confirmed (2026-06-02)
- [x] `include/i2c_ids.h` — `CMD_LOCO_DRIVE` (0x10), `CMD_LOCO_STOP` (0x11),
      `CMD_LOCO_SENSORS` (0x12), `LOCO_BRIDGE_ADDR` (0x42)
- [x] `modules/locomotion/LocoProtocol.h` — pure shared wire format (drive payload +
      12-byte `LocoSensors` snapshot; pack/unpack the single source of truth)
- [x] `modules/locomotion/LocomotionModule.h` — Pico master (`drive`/`stop`/`loco
      sensors` via `hal_i2c_*`); `cmdr module enable locomotion` (pico/pico2).
      New `robot/` (pico2_w) consumer project **builds clean** (`robot.uf2`)
- [x] `modules/locomotion/LocomotionBridge.h` — R4 I2C-slave bridge (Wire slave →
      shared `Roomba` driver); ISR-safe (latch + cached snapshot, blocking I/O in
      `tick()`). `cmdr module enable loco-bridge` (r4, supersedes `roomba`).
      Compiles + links on the R4 toolchain
- [x] `modules/I2CDiagModule.h` — `i2c scan`/`read`/`write` diagnostic module
      (`cmdr module enable i2c`, all platforms) for bringing up the bridge:
      `i2c scan` finds 0x42, `i2c write 0x42 0x10 …` pokes a raw drive, `i2c read
      0x42 0x12 12` dumps the snapshot
- [x] **Hardware test:** Pico 2 W ↔ R4 (I2C) ↔ Roomba — the bridge works; the Pico
      shell drives a real robot through the R4 over I2C (2026-06-02)

#### Phase R3 — Bluetooth controller — in progress (2026-06-02)
- [x] Host decided: **Pico 2 W native** (CYW43 Bluetooth via Bluepad32/BTstack)
- [x] Generic, backend-agnostic controller plumbing in `modules/controller/`
      (poll `state()` / push `onUpdate`/`onButton` / declarative `bind` — robot-free)
- [x] BT-only de-risk **hardware-confirmed**: Bluepad32+BTstack under commander's
      FreeRTOS on a Pico 2 W; a paired pad's left stick drove the robot through the
      R4 bridge (`~/github/bt-test`, standalone proving ground)
- [x] Rolled into commander: `platform/pico/` Bluepad32 backend, `cmdr module
      enable controller` (pico/pico2 — injects CYW43_ENABLE_BLUETOOTH +
      COMMANDER_ENABLE_CONTROLLER, builds the `commander_pico_controller` target),
      and the runner owns one `cyw43_arch_init()` shared by WiFi + BT
- [x] WiFi + BT combined: falls out of the single-init runner — the `robot`
      project builds with WiFi creds **and** the controller module in one firmware
- [ ] Hardware test: re-confirm a pad drives the robot from the rolled-in
      `controller` module (and telnet still works alongside BT)

### What's next

1. **Phase R3 — Bluetooth controller** — generic `modules/controller/` plumbing is
   in, and the BT-only Bluepad32 backend is hardware-confirmed (a pad drove the
   robot via the R4 bridge). Next: WiFi+BT combined on the one CYW43, then roll the
   Pico backend into `platform/pico/` + a `cmdr` `controller` module.
2. **OTA hardware test** — pico & esp32 runners already register the pull-based
   `ota <url>` command (gated by COMMANDER_ENABLE_OTA via `cmdr enable ota`);
   exercise it end-to-end on Pico W / Pico 2 W / ESP32 hardware.
3. **Board commands** — `reboot-bootloader` on Pico (reset_usb_boot); equivalent
   on ESP32 (esp_restart into download mode or DFU).
4. **ESP32 module system + IR** — wire ESP32 into the `cmdr module` system (hook
   main + target detection) and add an RMT-based IR impl.
5. **Bluepill I2C** — implement `hal_i2c_*` for the STM32 (I2C1 peripheral or
   bit-bang) to bring up `compass`; currently stubbed in `hal/stm32/hal.cpp`.

## Board pin reference

| Signal    | Arduino Uno | Pico W (Grove) | Pico 2 W (Grove) | ESP32-S3-N16R8 |
|-----------|-------------|----------------|------------------|----------------|
| I2C SDA   | A4          | GP4            | GP4              | GPIO4          |
| I2C SCL   | A5          | GP5            | GP5              | GPIO5          |
| Sonar     | D4          | GP6            | GP6              | —              |
| IR recv   | D5          | GP22 (PIO)     | GP22 (PIO)       | — (RMT todo)   |
| UART TX   | —           | GP20 (stdio)   | GP20 (stdio)     | GPIO43         |
| UART RX   | —           | GP21 (stdio)   | GP21 (stdio)     | GPIO44         |
