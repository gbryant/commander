# commander — project plan

> **Forward-looking framework directions** (cmdr test suite + build-matrix CI, the
> console/channel *session* unification, binary channel framing, a HAL capability
> model) live in [`docs/roadmap.md`](docs/roadmap.md). This file tracks current
> architecture + status.

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
│  runners/esp32/  arduino-uno/  arduino-r4/            │   FreeRTOS tasks,
│  stm32-bluepill/  zephyr/ (one per target family)     │   panic hooks
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
    GIT_TAG        v1.0     # pin a release; `cmdr pin` manages this
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
| `hal/zephyr/`                     | 🟡 partial   | Uno Q M33: UART (console + channel bus) + devicetree IR; GPIO/I2C stubbed |
| `runners/zephyr/`                 | ✅ done      | Uno Q M33 runner (west build, openocd-over-adb flash); registers no board commands yet |
| `platform/btt-tft35/`             | 🔲 scaffolded | BTT TFT35-E3 V3.0 (STM32F207VC); headless-shell scope only, no hardware yet — see "BTT TFT35-E3 port" below |
| `hal/stm32f2/`                    | 🟡 partial   | CMSIS regs (MODER/AFR style, not F1's CRL/CRH): GPIO/time/UART1(PA9/PA10) done, unverified; I2C stubbed |
| `runners/btt-tft35/`              | ✅ done      | `COMMANDER_TFT35_RUNNER` hook main, mirrors the Bluepill runner minus USB/DFU |
| **CMake library targets**         | ✅ done      | `commander::core/hal_pico/transport_*/modules`      |
| **`runners/pico/`**               | ✅ done      | `commander::pico_runner`; owns main(), WiFi, hooks  |
| **`commander.h` API**             | ✅ done      | `CommanderConfig`, required + optional callbacks    |
| **FetchContent validation**       | ✅ done      | scratch project at `/tmp/commander-test-app` builds |
| **`commander_generate_scripts()`**| ✅ done      | generates bum/build/upload/monitor/bum-ota scripts  |
| **`runners/esp32/`**              | ✅ done      | IDF component; UART + WiFi + Telnet confirmed       |
| **`runners/arduino-r4/`**         | ✅ done      | FreeRTOS; UART + WiFi + Telnet + mDNS confirmed     |
| **`cmdr` tool**                   | ✅ done      | pip install; `init` uno/r4/pico/pico2/esp32/**bluepill**; `--chip/--flash/--psram`; `pull`/`update`/`config`; `link`/`unlink` (local checkout); `pin`/`unpin` (lock commander version) |
| **`cmdr module` system**          | ✅ done      | enable/disable/list; cmdr.toml manifest; generates `commander_modules.h`; system/compass/sonar/ir/roomba |
| **`cmdr` features + tooling**     | ✅ done      | optional build-flag features (e.g. IR `wall`); per-module host tools in `bin/` + seed dirs; VID/PID port detection |
| **LittleFS — ESP32**              | ✅ done      | `cmdr enable littlefs [--size/--label/--dir]`; composable partitions (stacks with OTA, `disable ota` keeps FS); esp_littlefs git dep; `commander_mount_littlefs()`; HW-confirmed via cmdr-ipstube |
| OTA — R4 (on-demand)              | ✅ done      | `cmdr enable ota`; `ota start` hands off telnet→OTA; push confirmed on hardware (2026-05-29) |
| OTA — Pico (pull `ota <url>`)     | ✅ done      | `cmdr enable ota`; **HW-confirmed 2026-08-23 on Pico W (RP2040) and Pico 2 W (RP2350)** — build bumped over WiFi via `ota_push.py`, each verified independently over telnet |
| OTA — ESP32 (pull `ota <url>`)    | ✅ done      | `cmdr enable ota`; HW-confirmed via cmdr-ipstube `bum-ota` (2026-06); weak `commander_on_ota_*` display hooks; project-level `commander_stamp_version()` so `bum-ota` confirms the build |
| DFU upload — Bluepill             | ✅ done      | `cmdr enable dfu`; davidgfnet bootloader (`bootloader` cmd + dfu-util); HW-confirmed |
| Board commands — Pico             | ✅ done      | `runners/pico/BootselModule` registers `reset` + `bootloader` (reset_usb_boot); Bluepill has `bootloader` too (USB-DFU) |
| `modules/ir/IIRModule.h`          | ✅ done      | interface only                                      |
| `platform/pico/IRModule` (PIO)    | ✅ done      | PicoIRModule (PIO+core1); in cmdr module system (`cmdr module enable ir`) |
| `platform/arduino/IRModule`       | ✅ done      | IRremote-based; in cmdr module system on Uno + R4 (`cmdr module enable ir`) |
| `platform/esp32/Esp32IRModule` (RMT) | ✅ done   | in cmdr module system (2026-06-18); needs hardware test |
| `platform/stm32-bluepill/Stm32IRModule` | ✅ done | EXTI/DWT capture (2026-06-18); needs hardware test  |
| `modules/roomba/Roomba`           | ✅ done      | portable OI driver via abstract `RoombaPort`        |
| `modules/roomba/RoombaModule`     | ✅ done      | `oi` shell command; drove a real Roomba from R4     |
| Bluetooth controller module       | ✅ done      | `modules/controller/` + Bluepad32 Pico backend (`cmdr module enable controller`); see Phase R3 |

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
- [x] Extend the module system + IR to ESP32 (RMT) and Bluepill (EXTI/DWT) —
      landed 2026-06-18; both need a hardware pass.

### Phase R — robot integration

Goal: migrate Roomba robot to this framework.
- Pico 2 W = main controller (commander shell + FreeRTOS)
- Arduino R4 = Roomba OI bridge (I2C slave → Serial1 → Roomba)
- BT controller TBD

#### Phase R0 — platform proofs ✅ done (2026-05-29)
- [x] `platform/arduino-r4/` builds (WiFi + OTA + Telnet + UART shell)
- [x] `platform/pico2/` builds and runs (RP2350 INVPC fault fixed 2026-05-25)
- [x] **Flash R4 and confirm `help` + WiFi + Telnet** — done via `runners/arduino-r4`;
      also `mDNS` (resolves; telnet-by-name confirmed) (2026-05-29). That project was
      renamed `cmdr-oi-bridge`.

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

#### Phase R3 — Bluetooth controller — DONE, hardware-confirmed (2026-06)
- [x] Host decided: **Pico 2 W native** (CYW43 Bluetooth via Bluepad32/BTstack)
- [x] Generic, backend-agnostic controller plumbing in `modules/controller/`
      (poll `state()` / push `onUpdate`/`onButton` / declarative `bind` — robot-free)
- [x] BT-only de-risk **hardware-confirmed**: Bluepad32+BTstack under commander's
      FreeRTOS on a Pico 2 W; a paired pad's left stick drove the robot through the
      R4 bridge (proved in a standalone throwaway project, not published)
- [x] Rolled into commander: `platform/pico/` Bluepad32 backend, `cmdr module
      enable controller` (pico/pico2 — injects CYW43_ENABLE_BLUETOOTH +
      COMMANDER_ENABLE_CONTROLLER, builds the `commander_pico_controller` target),
      and the runner owns one `cyw43_arch_init()` shared by WiFi + BT
- [x] WiFi + BT combined: falls out of the single-init runner — the `robot`
      project builds with WiFi creds **and** the controller module in one firmware
- [x] **Hardware test:** a pad drives the robot from the rolled-in `controller`
      module, and telnet works alongside BT on the one CYW43 — confirmed while
      building the `cmdr-robot` drive glue (50 Hz ticker, STOP resend, spin
      wiring, `drivedbg`), which is what shook out drive creep and the
      arc-only steering rule

### BTT TFT35-E3 V3.0 port (scaffolded, not yet hardware-confirmed)

A new target board: the BIGTREETECH TFT35-E3 V3.0 touchscreen (STM32F207VC, Cortex-M3,
256 KB flash, 128 KB SRAM), from the same MCU family the [BTT TouchScreenFirmware]
(https://github.com/bigtreetech/BIGTREETECH-TouchScreenFirmware) project targets — this
port does not use or depend on that firmware, only the same hardware. Scope decided
2026-08-26: **headless shell only**, matching the Bluepill precedent — no LCD/touch/SD
driver yet. That would be a much larger follow-up (display controller bring-up, font/
bitmap rendering, touch calibration) and is deliberately out of scope for the first pass.

- [x] `hal/stm32f2/hal.cpp` — GPIO, DWT time base, USART1 console (PA9/PA10, AF7), I2C
      stub. Modeled on `hal/stm32/hal.cpp` but ported to F2's MODER/OTYPER/OSPEEDR/PUPDR/
      AFR GPIO block (shared with F4), not F1's CRL/CRH nibble style.
- [x] `platform/btt-tft35/clock.c` — deliberately conservative: runs from the internal
      16 MHz HSI, no PLL, no assumption about this board's HSE crystal (unconfirmed).
      Swap to HSE+PLL (like the Bluepill's 72 MHz bring-up) once the board's crystal is
      known — that also needs PWR/voltage-scaling + ART accelerator + flash wait-state
      tuning that 16 MHz doesn't require.
- [x] `runners/btt-tft35/runner.cpp` + `FreeRTOSConfig.h` — mirrors
      `runners/stm32-bluepill/`, no USB/DFU (not scoped yet).
- [x] `platformio.ini` `[env:btt-tft35]` + `scripts/stm32_tft35_freertos.py` (Cortex-M3
      GCC/ARM_CM3 port, same as Bluepill).
- [ ] **Before first flash attempt**, confirm against the actual board/schematic:
      1. Whether PA9/PA10 are broken out on a header for USART1, or reserved for the
         LCD/touch bus — may need a different UART.
      2. HSE crystal presence/frequency, to enable PLL bring-up.
      3. The onboard status LED GPIO, to wire into `commander_on_panic()` (currently a
         silent halt — see `platform/btt-tft35/stm32_panic.h`).
      4. `board = genericSTM32F207VC` in `platformio.ini` resolves in the installed
         `ststm32` platform version; the upstream BTT firmware's own `platformio.ini`
         uses a custom board JSON (`STM32F207VC_0x8000`) we don't currently vendor.
- [ ] First hardware test: ST-Link flash, confirm `help` over USART1.
- [ ] Bluepill I2C and this board's I2C stub are both open — likely worth landing
      STM32 I2C once, shared across `hal/stm32` and `hal/stm32f2` if the peripheral is
      similar enough.
- [ ] LCD/touch/SD as a later phase, once the shell boots on hardware.

### Testing

A tiered test suite landed (2026-06-16) — see `docs/testing.md`. Quick reference:

- `tests/run.sh` — Tier 0 (host C++: codec, NEC/Sony, CommandRegistry, DriveMixer,
  ControllerCalibration, broker loopback, codec↔broker byte-compat) + Tier 1
  (`tools/cmdr/tests/` pytest: golden `commander_modules.h` snapshots, codegen
  invariants, honest-menu gating, manifest/partition round-trips, scaffold +
  enable/disable idempotence). The seconds-fast pre-commit gate.
- `tests/build-matrix.sh` — Tier 2/3 compile smoke: `cmdr init`s throwaway projects
  against the local checkout and runs their real build scripts. Toolchain-detecting,
  skip-with-notice. `--full` for every config; `--list` to see the matrix.

### Channel bus (roadmap #2 — session unification)

Phases **A + B1 shipped (2026-06-17)** — see `docs/channels-first-class.md`. Uno Q +
tooling only; the six UART boards are untouched.

- **A — channel identity:** `include/channel_ids.h` is the single authority for channel
  ids + roles (the `i2c_ids.h` model: id, name, dir, kind, `command_session`). The broker
  mirrors it and derives its channel set from it; `test_channel_ids_sync.py` guards drift.
- **B1 — command sessions:** `ChannelTransport::route()` dispatches any `command_session`
  channel on its own writer, so multiple host processes get isolated shells over one link
  (ch0 unchanged). Replies frame back on the originating channel.

**Autostart (2026-06-17)** — `cmdr autostart add|remove|list|clear` records boot command
lines in `cmdr.toml` `[autostart]`; the generated `commander_run_autostart(reg)` (called by
every runner after the ready-hook) dispatches them via a reusable `core/NullWriter`. Universal
(any command, all platforms; empty = no-op weak default, so the AVR tier pays nothing); *stop*
is the command's own toggle. Enables the zero-code Uno Q IR demo: `cmdr autostart add "ir recv"`
→ a fresh board streams presses with no command sent.
- **Deferred (by design):** B2 (collapse `UartTransport`/ch0-console — the only part that
  reaches the AVR tier, gated on a flash+RAM size diff), C (channels on more boards — the
  prerequisite for a multi-consumer Pico), D (cmdr channel modeling), E (handshake/binary).
  Protocol versioning was **declined** (self-contained, matched-pair deploy + the build-time
  codec↔broker test); a one-byte mismatch tripwire is the deferred fallback.

### What's next

1. **Board commands — ESP32 only.** Pico has `reset` + `bootloader`
   (`BootselModule`, reset_usb_boot) and the Bluepill has `bootloader` (USB-DFU).
   The ESP32 has `reset` but no way to drop into download mode from the shell.
2. **IR hardware pass** — the ESP32 (RMT) and Bluepill (EXTI/DWT) IR modules
   landed 2026-06-18 in the module system; exercise both on hardware.
3. **Bluepill I2C** — implement `hal_i2c_*` for the STM32 (I2C1 peripheral or
   bit-bang) to bring up `compass`; currently stubbed in `hal/stm32/hal.cpp`.
4. **Grove Vision AI V2 (`aicam`)** — esp32 module landed: SSCMA AT protocol over a
   UART/I2C transport seam (`modules/aicam/` + `platform/esp32/AiCamUartTransport`),
   host = XIAO ESP32-S3, consumer = `cmdr-ai-cam` (repo not yet published). Added
   `hal_i2c_read_raw` to the
   HAL. **HW-confirmed (2026-06-11): full pipeline** — `aicam info`/`sensors` plus live
   `aicam stream` inference (rock-paper-scissors model) over the UART link. Still to
   exercise on HW: `snap` (640x480 image may exceed AICAM_RX_MAX) and the I2C transport.

## Board pin reference

These are the defaults `cmdr module enable` offers; every one is overridable at
enable time and recorded in the project's `cmdr.toml`.

| Signal    | Arduino Uno | Pico W | Pico 2 W | ESP32-S3-N16R8 |
|-----------|-------------|--------|----------|----------------|
| I2C SDA   | A4          | GP6    | GP6      | GPIO4          |
| I2C SCL   | A5          | GP7    | GP7      | GPIO5          |
| Sonar     | D6          | GP6 ⚠  | GP6 ⚠    | GPIO6          |
| IR recv   | D5          | GP22 (PIO)     | GP22 (PIO)       | GPIO38 (RMT)   |
| UART TX   | —           | GP20 (stdio)   | GP20 (stdio)     | GPIO43         |
| UART RX   | —           | GP21 (stdio)   | GP21 (stdio)     | GPIO44         |

⚠ The sonar default (pin 6 on every target) collides with the Pico's I2C SDA
default. Enabling both on a Pico means changing one at enable time.
