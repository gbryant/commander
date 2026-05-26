# commander — project plan

## Goals

A portable embedded command shell that runs the same module code across multiple
target boards. Hardware modules (sensors, actuators) are developed and validated
on the Arduino Uno testbed, then deployed unchanged to Pico W and ESP32 targets
using their native SDKs — no Arduino core on production targets.

## Principles

- No Arduino framework on Pico or ESP32. Use native SDKs (Pico SDK, ESP-IDF).
- FreeRTOS throughout — it ships with both Pico SDK and ESP-IDF, same task API.
- HAL isolates platform code. Modules depend only on `hal/hal.h` and `core/`.
- `i2c_ids.h` is the wire protocol. It must stay identical across all platforms.
- IR is platform-specific (IRremote / PIO / RMT) — implement `IIRModule` per target.

## Architecture

```
┌──────────────────────────────────────────────────┐
│  platform/arduino   platform/pico   platform/esp32│  ← main, FreeRTOS config,
│  (main + task setup per board)                   │    board pin assignments
├──────────────────────────────────────────────────┤
│  transport/uart          transport/telnet         │  ← how commands arrive
│  (UartTransport)         (TelnetTransport)        │    (serial / WiFi)
├──────────────────────────────────────────────────┤
│  modules/                                         │  ← sensor & system modules
│  CompassModule  SonarModule  IIRModule  ...       │    zero platform code
├──────────────────────────────────────────────────┤
│  hal/  arduino/hal.cpp  pico/hal.cpp  esp32/hal   │  ← I2C, GPIO, UART, time
│  (one .cpp compiled per target, same interface)   │
├──────────────────────────────────────────────────┤
│  core/                                            │  ← pure C++, no platform
│  IModule  CommandRegistry  Writer  SystemModule   │
├──────────────────────────────────────────────────┤
│  include/i2c_ids.h                               │  ← wire protocol spec
└──────────────────────────────────────────────────┘
```

## Status

| Area                        | State       | Notes                                      |
|-----------------------------|-------------|--------------------------------------------|
| `core/` — registry, writer  | ✅ done     |                                            |
| `include/i2c_ids.h`         | ✅ done     | merged from nano + micro                   |
| `hal/hal.h` interface        | ✅ done     | I2C, GPIO, time, UART                      |
| `hal/arduino/`              | ✅ done     | Wire + Arduino GPIO + Serial               |
| `hal/pico/`                 | ✅ done     | Pico SDK                                   |
| `hal/esp32/`                | ✅ done     | ESP-IDF v5 i2c_master + UART driver        |
| `core/SystemModule.h`       | ✅ done     | `help` command                             |
| `modules/CompassModule.h`   | ✅ done     | HAL only, no Wire.h                        |
| `modules/SonarModule.h`     | ✅ done     | pin passed at construction                 |
| `transport/uart/`           | ✅ done     | task body platform-agnostic; platform main owns stack size |
| `platform/arduino/FreeRTOSConfig.h` | ✅ done | project-owned; library version overridden by patch script |
| `scripts/patch_freertos.py` | ✅ done     | disables timer task, reduces idle stack; runs pre-build   |
| `bum-uno`                   | ✅ done     | build + upload + monitor in one command    |
| `bum-pico`                  | ✅ done     | cmake build + uf2 copy + tio monitor       |
| `platformio.ini` (root)     | ✅ done     | `src_dir=.`, explicit src filter, runs from commander/    |
| `platform/arduino/`         | ✅ done     | builds clean: 51% RAM, 53% flash on Uno   |
| `platform/pico/`            | ✅ done     | builds clean; `help` confirmed over USB CDC |
| `platform/esp32/`           | ✅ done     | builds clean; `help` confirmed over native USB CDC (USB Serial/JTAG) |
| `hal_i2c_probe()`           | ✅ done     | Wire endTransmission / i2c_read_blocking / i2c_master_probe per platform |
| `modules/I2cModule.h`      | ✅ done     | `scan` confirmed on ESP32-S3 (found 0x40, 0x41) |
| `modules/ir/IIRModule.h`   | ✅ done     | interface only                             |
| `platform/arduino/IRModule` | ⬜ todo     | port from nano-commander (IRremote)        |
| `platform/pico/IRModule`    | ⬜ todo     | PIO-based implementation                   |
| `platform/esp32/IRModule`   | ⬜ todo     | RMT-based implementation                   |
| `transport/telnet/`         | ✅ done     | lwIP (Pico/ESP32) + ArduinoTelnet (R4)    |
| `platform/arduino-r4/`      | ✅ builds   | WiFi + OTA + Telnet; needs hardware test   |
| `platform/pico2/`           | ✅ builds   | RP2350 via -DPICO_BOARD=pico2_w; needs hw test |
| Roomba driver module        | ⬜ todo     | modules/roomba/ — OI protocol, hal_uart_* |
| Bluetooth module            | ⬜ todo     |                                            |

## Roadmap

### Phase 1 — serial shell on all three targets (current)
- [x] Scaffold repo structure
- [x] Core, HAL, shared modules
- [x] `UartTransport` + `SystemModule`
- [x] `platform/arduino` builds clean (51% RAM, 53% flash)
- [x] `scripts/patch_freertos.py` + `bum-uno` in place
- [x] Upload to Uno and confirm `help` works over serial
- [x] Verify `platform/pico` builds and `help` works over USB CDC
- [x] Verify `platform/esp32` builds and `help` works over native USB CDC

### Phase 2 — sensor modules on Pico
- [ ] Prove `CompassModule` and `SonarModule` work unchanged on Pico
- [ ] Add Pico-native IR module (PIO) implementing `IIRModule`

### Phase 3 — telnet transport on Pico W
- [x] Port `TelnetTransport` from micro-commander (lwIP BSD sockets)
- [x] Wire into `platform/pico/main.cpp` alongside UART

### Phase 4 — ESP32
- [ ] Verify sensor modules on ESP32
- [ ] Add ESP32-native IR module (RMT) implementing `IIRModule`
- [ ] WiFi + Telnet transport for ESP32

### Phase 5 — retire nano-commander and micro-commander
- [ ] Confirm all nano-commander functionality covered
- [ ] Confirm all micro-commander functionality covered

### Phase R — robot integration (new)

The goal is to migrate the Roomba robot project to this framework with:
- Pico 2 W as the main controller (running commander + FreeRTOS)
- Arduino R4 as the Roomba bridge (I2C slave → Roomba OI over Serial1)
- Bluetooth controller TBD (ESP32, dedicated Pico W, or native Pico 2 W BLE)

#### Phase R0 — platform proofs (current)
- [x] `platform/arduino-r4/` builds (WiFi + OTA + Telnet + UART shell)
- [x] `platform/pico2/` builds (RP2350 via `-DPICO_BOARD=pico2_w`)
- [ ] Flash R4 and confirm `help` + telnet work
- [x] Flash Pico 2 W and confirm `help` + WiFi + Telnet work
      Root cause of FreeRTOS hang: missing `configRUN_FREERTOS_SECURE_ONLY 1` in
      FreeRTOSConfig.h — RP2350_ARM_NTZ port used non-secure EXC_RETURN (0xFFFFFFBC,
      bit0=0) causing INVPC UsageFault on first context switch. Fixed 2026-05-25.

#### Phase R1 — Roomba driver module
- [ ] `modules/roomba/Roomba.h` — OI protocol driver using `hal_uart_*` (no Arduino APIs)
- [ ] Wire into `platform/arduino-r4/main.cpp` (Arduino speaks OI on Serial1)
- [ ] `i2c_ids.h` — add Roomba bridge command/sensor registers

#### Phase R2 — Pico 2 W as Roomba bridge master
- [ ] Arduino R4 becomes I2C slave (accepts roomba commands, returns sensor data)
- [ ] Pico 2 W `RoombaModule` talks to Arduino bridge via `hal_i2c_*`
- [ ] Basic drive + stop commands working from Pico shell

#### Phase R3 — Bluetooth controller
- [ ] Decide: dedicated Pico W slave / Pico 2 W native BLE / ESP32
- [ ] Controller input → locomotion commands

## Board pin reference

| Signal    | Arduino Uno | Pico W (Grove) | ESP32-S3-N16R8 |
|-----------|-------------|----------------|----------------|
| I2C SDA   | A4          | GP4            | GPIO8          |
| I2C SCL   | A5          | GP5            | GPIO9          |
| Sonar     | D4          | GP6            | GPIO4          |
| IR recv   | D5          | GP7 (PIO)      | GPIO5 (RMT)    |
| UART TX   | —           | —              | GPIO43 (fixed) |
| UART RX   | —           | —              | GPIO44 (fixed) |
