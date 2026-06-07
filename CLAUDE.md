# commander — Claude context

## What this is

A portable embedded command shell targeting Arduino Uno (testbed), Raspberry Pi
Pico W, ESP32, and the STM32 "Bluepill" (F103). Same module code (compass, sonar,
IR, etc.) runs on all of them. Platform-specific code is limited to `hal/`,
`transport/`, and `platform/`. See `PLAN.md` for roadmap and status.

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

Per-board dev scripts live under `dev/<board>/` — `build`, `bum` (build+upload+
monitor), `upload`, `monitor`, and `bum-ota` where supported (e.g. `dev/pico/bum`,
`dev/esp32/build`). `ls dev/<board>/` shows what's available for each target.

### Arduino Uno
```bash
dev/uno/bum          # build + upload + monitor in one command
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

OTA differs by platform. **Pico and ESP32 use a pull model** instead: their
runners register an `ota <url>` command (gated by `COMMANDER_ENABLE_OTA`, set by
`cmdr enable ota` on the CMake side) that downloads firmware from a URL and
self-flashes (pico_fota_bootloader / esp_ota). lwIP has plenty of sockets there,
so no Telnet hand-off is needed. Both are wired in the runners but not yet
hardware-tested. R4's push model is the exception, forced by WiFiS3's socket cap.

### Pico W
Build system is CMake + Pico SDK. `pico_sdk_import.cmake` and
`FreeRTOS_Kernel_import.cmake` are checked in at the repo root.

```bash
dev/pico/bum           # build + upload + monitor in one command
dev/pico/build         # cmake build only
```

### Pico 2 W (RP2350)
Same CMakeLists as Pico W — board is overridden via `-DPICO_BOARD=pico2_w`.
Uses a separate build directory (`platform/pico/build-pico2/`).

```bash
dev/pico2/bum          # build + upload + monitor in one command
dev/pico2/build        # cmake build only
```
BOOTSEL volume is `/Volumes/RP2350` (vs `/Volumes/RPI-RP2` on RP2040).
`FreeRTOSConfig.h` auto-detects `PICO_RP2350` and enables dual-core SMP,
Cortex-M33 FPU, and 200 KB heap (vs 128 KB on RP2040).

### ESP32-S3-N16R8
Uses ESP-IDF v5. Project root is `platform/esp32/`; component sources are in `platform/esp32/main/`.

```bash
dev/esp32/bum          # build + upload + monitor in one command
dev/esp32/build        # build only (runs set-target on first run)
dev/esp32/upload       # flash via esptool (auto-detects port)
dev/esp32/monitor      # tio at 115200 baud
```

First build runs `idf.py set-target esp32s3` automatically (detects missing `sdkconfig`).
`sdkconfig.defaults` configures 16 MB flash and 8 MB OPI PSRAM for the N16R8 variant.
Transport uses native USB (USB Serial/JTAG, GPIO19/20) — connect tio to the `usbmodem` port.
The board has no USB-to-serial chip; UART0 (GPIO43/44) is on headers only.

**ESP-IDF environment:** run `esp` before any `idf.py` or `dev/esp32/*` command.
`esp` is a shell alias for `. ~/u-developer/esp-idf/export.sh`.

### STM32 Bluepill (STM32F103C8)
Native CMSIS + vendored FreeRTOS (GCC/ARM_CM3) + raw TinyUSB — **not** the Arduino
framework (no STM32duino). Flashed via an ST-Link clone (SWD). The build pulls the
kernel and USB stack from `$FREERTOS_KERNEL_PATH` and a TinyUSB checkout
(`$TINYUSB_PATH`, or `$PICO_SDK_PATH/lib/tinyusb`) — see `scripts/stm32_freertos.py`
and `scripts/stm32_tinyusb.py` (they `env.BuildSources` the external trees).

```bash
dev/bluepill/bum                # build + upload (USART1) via ST-Link
pio run -e bluepill           # USART1 console (PA9 TX / PA10 RX)
pio run -e bluepill-usb       # USB CDC console (raw TinyUSB, fsdev port)
pio run -e bluepill-usb-dfu   # USB CDC, app @ 0x08001000 (runs above the DFU bootloader)
```

`hal/stm32/hal.cpp` is the HAL (CMSIS registers: GPIO via CRL/CRH, DWT µs time base,
USART1; **I2C is stubbed** — compass not yet supported). `platform/stm32-bluepill/`
holds `clock.c` (HSE→72 MHz, USB 48 MHz), `usb.c`/`usb_descriptors.c` (TinyUSB CDC),
and the offset linker `stm32f103c8_dfu.ld`.

**USB-DFU upload (no ST-Link).** `dev/bluepill/flash-bootloader` installs the davidgfnet
DFU bootloader (GPL-3.0; an external dep under `~/u-developer/stm32-dfu-bootloader` like
the other SDKs — cloned by `~/u-developer/setup.sh`, overridable via
`$STM32_DFU_BOOTLOADER_PATH` — patched for macOS, no WinUSB, and to release D+ after its
re-enum nudge) once via ST-Link. Then `bootloader`
(a shell command, gated by `-DCOMMANDER_STM32_DFU`) reboots into DFU and
`dev/bluepill/upload-usb` flashes over USB with `dfu-util` (DfuSe `-s 0x08001000:leave`).
`dev/bluepill/unlock` clears RDP on clones that ship read-protected.

**D+ pull-up caveat.** Many Bluepill clones have a too-weak D+ pull-up (R10 ~10k instead
of 1.5k), so USB may only enumerate after pressing RESET post-plug. Real fix: add a
1.5k–1.8k resistor PA12→3.3 V.

## Modules (`cmdr module`)

Modules are composed by the `cmdr` tool, not by hand-editing `commander_setup()`.
`cmdr module enable <name>` asks the module's config questions, records them in a
per-project `cmdr.toml`, and regenerates `commander_modules.h` — a cmdr-owned
file (in `src/` for R4/Uno, project root for Pico, `main/` for ESP32) that
includes only enabled modules, constructs them with the saved answers (incl. any
board-specific adapter), and exposes `commander_register_modules(reg)`. Identical
registration lines are deduped, so several I2C modules (e.g. `compass` + `i2c` +
`locomotion`) share a single `hal_i2c_init` bring-up. The app's
`main.cpp` just calls that hook, so disabled modules aren't compiled (no flags).
Available: `system` (always), `compass` (HAL I2C — its emitter calls
`hal_i2c_init`), `sonar` (HAL GPIO, one pin), `i2c` (bus diagnostics —
`i2c scan`/`read`/`write` over `hal_i2c_*`, one command slot, all platforms;
handy for bringing up the locomotion bridge), `ina219` (INA219 current/power
monitor(s) over HAL I2C, all platforms — one namespaced `ina` command for however
many sensors are wired: `channels` at enable is a comma list of `label:addr`, e.g.
`a:0x40,b:0x45`; `ina` lists, `ina <ch> volt|amp|watt|stats|init`, `ina stats` dumps
CSV per channel. Calibrated for a 0.1 Ω shunt; the solar-monitor consumer logs
`ina stats`), `wifi` (Pico/Pico 2 W/R4/ESP32 —
`wifi status`/`off`/`on`; portable command over runner-implemented hooks
`commander_wifi_status/off/on` in `core/WifiHooks.h`, since WiFi/credentials and,
on the R4, the single modem-owning task live in the runner — R4 reads a cache +
sets request flags processed in `net_poll`, `wifi off` also suppresses
auto-reconnect; the ESP32 runner implements the hooks over `esp_wifi`, with
`wifi off` suppressing the disconnect-handler reconnect), `ir` (Pico via `PicoIRModule` PIO+core1 — the
`commander_pico_ir` CMake target encapsulates the PIO build; Uno via the
Uno & R4 via the IRremote-based `platform/arduino/IRModule`, unity-included by
the generated file with `IRremote` added to `lib_deps`). IR commands are namespaced consistently on
both platforms: `ir recv` (NEC/Sony), `ir wall` (Roomba virtual wall), `ir diag`
(Pico). `roomba` (R4 only — `Serial1` adapter). `locomotion` (Pico/Pico 2 W only —
master side: `drive`/`stop`/`loco sensors` to a remote mobile base over `hal_i2c_*`)
and `loco-bridge` (R4 only — the matching I2C-slave bridge that forwards
`CMD_LOCO_*` to a Roomba; it owns the shared `Roomba` driver and also provides
`oi`, so it's mutually exclusive with `roomba`). The bridge's I2C port is
selectable at enable (`port` question): `Wire1` = the Qwiic/STEMMA connector
(IIC0, **3.3 V** — wire a 3.3 V Pico master straight in, the default) or `Wire`
= the A4/A5 header pins (IIC1, **5 V** — needs a level shifter). Both R4 Qwiic
and A4/A5 map to hardware IIC peripherals, so both support I2C **slave** mode; an
SCI-backed Wire would not (`TwoWire::_begin` refuses a slave on SCI). The two locomotion sides share the
pure wire format in `modules/locomotion/LocoProtocol.h` (the I2C "register" is the
`CMD_LOCO_*` byte from `i2c_ids.h` — no extra framing); the bridge's Wire
`onReceive`/`onRequest` run in ISR context, so they only latch the command + serve a
pre-cached snapshot, with the blocking Roomba I/O done in `tick()` (a UART ticker).
The bridge reads the base **lazily** — only when the master asks (`loco sensors`
does a register-write to request a refresh, waits, then reads the freshened cache),
never on a free timer. A free-running poll both stuttered the drive stream (the
blocking Serial1 read stalls `tick()`) and kept the base awake forever (battery
drain). For **idle power**, after `LOCO_IDLE_MS` idle the bridge parks the base (OI
Stop → motors off, charging/sleep allowed); the next drive re-inits it via
`Roomba::start()`. If a BRC wake line is wired (`brc` at `cmdr module enable
loco-bridge`, Mini-DIN 5) that re-init pulses BRC to auto-wake a slept base; with no
BRC, a deep-slept base needs a manual button press to wake.
For analog driving, `modules/locomotion/DriveMixer.h` is a reusable, input-agnostic
helper that turns a normalized throttle+steer pair (e.g. from
`ControllerCalibration::apply`) into a smooth ramped `(velocity, radius)` loco
command — two-zone velocity/radius + velocity ramping (ported from the original
robot's smooth feel). The consumer just picks the stick layout and ships the result.
`update()` takes an optional `spin` direction arg (spin-in-place; speed follows the
same two-zone curve as driving on `|throttle|`, scaled by `spinScalePct` for a
slow/fine mode), and `Config.stickSpin=false` makes the steer stick arc-ONLY so a
tight turn can't trip an unwanted spin (the robot puts spin on the shoulder buttons:
hold one, left stick fwd/back = CW/CCW, R2 = normal speed / L2 = slow).
The master also gets a **remote console** over the same link: `bridge <cmd>` runs a
command on the bridge board's own shell (the slave dispatches it through its
`CommandRegistry` and streams the captured output back via `CMD_CONSOLE_EXEC`/`READ`)
and `bridge reset` hard-resets it (`CMD_RESET`) — so you can drive/triage/reboot the
R4 from the Pico's solid console even when the R4's own WiFi/telnet is unreachable.
`controller` (Pico/Pico 2 W only — Bluetooth game-controller input via Bluepad32 +
BTstack). It's a **generic input source**, not robot-specific: the module publishes
controller state three mix-and-match ways — poll (`state()`), push
(`onUpdate`/`onButton` C++ callbacks), and declarative `bind <button> <command…>`
(dispatches any registered command on press) — plus a `pad` status command. Apps
wire input to anything via the weak `commander_on_controller_ready(ControllerModule&)`
hook the generated file calls (e.g. the robot maps the left stick to `CMD_LOCO_DRIVE`).
The neutral vocabulary is `modules/controller/ControllerState.h`; the Bluepad32 Pico
backend is `platform/pico/` (`bp32_pico.c` C shim + `PicoBluepadBackend.h`) behind the
`ControllerBackend` seam. The module **calibrates** every sample before publishing
(`ControllerCalibration` — re-center/rescale/smooth-deadzone), so `state()`/`onUpdate`
give re-centered sticks; `rawState()` exposes the raw backend sample. Calibration is
spatial/stateless so it's correct at any report rate. **Temporal** smoothing
(`StickFilter`, a rate-independent EMA) is deliberately NOT in the module — a low-pass
must run at the consumer's fixed loop rate, not the intermittent report rate, or it
freezes between reports (causing drive creep); consumers apply `StickFilter` on
`rawState()` in their own tick (the robot does, in its 50 Hz drive ticker). The
`calibrate` command runs an interactive 4-phase routine (it samples
`rawState()`, suppresses input handlers while running, and fires the `onCalibrate`
hook so apps stop actuators — the robot sends `CMD_LOCO_STOP`). `ControllerCalibration`
ships a baked default profile (currently a Wii U Pro); other controllers `calibrate`
or `calibration().setIdentity()`. `btforget` clears stored BT bonds
(`uni_bt_del_keys_safe`) so a controller with a stale link key can re-pair (HCI auth
status=5 / L2CAP loop). Enabling it is heavy (BT firmware): `cmdr module enable
controller` injects `CYW43_ENABLE_BLUETOOTH=1` (before `pico_sdk_init`) and
`COMMANDER_ENABLE_CONTROLLER=ON` (before `FetchContent`) into the app CMake; the
runner then builds the opt-in `commander_pico_controller` target (needs
`BLUEPAD32_PATH`). The runner owns a single `cyw43_arch_init()` shared by WiFi + BT,
so WiFi (telnet/OTA) and a Bluetooth controller coexist on the one CYW43. (Static
libs link the cyw43_arch **`_headers`** target — the arch `.c` are INTERFACE sources;
only the executable links the full flavor.)
`ipstube` (ESP32 only — the six ST7789 135×240 IPS displays on the IPSTube clock).
A platform-specific display **driver**: `platform/esp32/IpstubeModule.{h,cpp}` brings
up one shared SPI bus with a **single** `esp_lcd` ST7789 panel and **manual GPIO
chip-select** for the six displays (per-display CS, left-to-right 13/12/14/27/2/15 so
display 0 = leftmost = hours-tens, driven active-low; an ESP32 SPI host has only 3
hardware CS slots, so 6 panel_io can't work — select-all inits all six in parallel,
select-one draws to a digit; **SPI mode 3** (HW-confirmed), shared DC=25/RST=26/MOSI=32/
SCLK=33; the TFT backlight is `TFT_ENABLE_PIN=GPIO4`, **active-low** PWM via LEDC —
GPIO5 is the separate WS2812 ambient LED chain, not the TFT backlight), exposing
`ipstube on/off/dim/fill/clear/test` + debug (`info/cs/reinit/invert/swap/mirror/gap/spi/rgb`)
plus a C++ API (`pushDigit`/`fill`/`backlight`). The IPSTube's 6 WS2812 ambient LEDs
(GPIO5) are driven by the separate `ws2812` module (below), enabled alongside `ipstube`.
The clock-face logic lives in the app via the weak `commander_on_ipstube_ready(IpstubeModule&)`
hook the generated file calls. The header keeps esp_lcd types out so the app can
include it freely; the `.cpp` compiles in the runner only when enabled — `cmdr module
enable ipstube` sets `COMMANDER_ENABLE_IPSTUBE` in the app CMake, which makes the runner
add the `.cpp` + `esp_lcd`/`esp_driver_spi`/`esp_driver_ledc` REQUIRES. Panel tunables
(invert, RGB/BGR, gap X/Y, mirror) and all pins are `-DIPSTUBE_*` overridable.
`ws2812` (ESP32 only — a generic WS2812/SK6812 addressable-RGB chain over the RMT
peripheral; `platform/esp32/Ws2812Module.{h,cpp}`). Questions: `pin`, `count`, colour
`order` (GRB default). Command `wled`: `wled <r> <g> <b>` (all), `wled <i> <r> <g> <b>`
(one pixel), `wled off`, `wled bright <0-255>`; the app drives effects via the weak
`commander_on_ws2812_ready(Ws2812Module&)` hook (`setPixel`/`fill`/`show`). A board's
onboard RGB LED is just this with `count=1`; multiple chains would follow the ina219
`channels` pattern. Enable injects `COMMANDER_ENABLE_WS2812` (gates the `.cpp` in the
runner); `esp_driver_rmt` is an unconditional runner REQUIRE. The IPSTube enables both
`ipstube` (displays) and `ws2812` (GPIO5 ambient) — separate peripherals, separate modules.
Cross-platform modules use the same emitter on every target; `ir`, `roomba`,
`locomotion`/`loco-bridge`, `controller`, `ipstube`, and `ws2812` are platform-gated, and a module may declare per-target question defaults (e.g.
IR pin 22 on Pico, 5 on Uno) and `pio_lib_deps` (PlatformIO libs to add on
enable). Uno is in the module system via a no-WiFi hook main.

A module may also declare optional **`features`** (default off) gated by a build
flag the tool injects — PlatformIO `build_flags` or a CMake
`add_compile_definitions`, kept consistent across all TUs (on Pico the IR header
compiles in two TUs, so an inconsistent define would be an ODR violation). e.g.
IR's Roomba virtual-wall detection (`ir wall`) is off unless you opt in at
`cmdr module enable ir`, so it costs no flash/RAM/command-slot otherwise (on the
Uno that's ~100 bytes RAM + a command slot). Code is gated with
`#ifdef COMMANDER_IR_WALL`; standalone reference builds define it to keep wall. A module whose `tick()` must be
pumped by the UART task (e.g. IR `recv`) also emits a strong
`commander_on_uart_ready()` into the generated file that `uart.addTicker()`s it
(overriding the runner's weak hook).

A module may also ship **companion host tooling** via `tools` (+ optional
`tool_dirs`) in its spec. `cmdr module enable` copies the tools (shipped as cmdr
templates) into the project's `bin/` (executable) and creates any `tool_dirs`;
`disable` removes the tools but preserves the data dirs (they may hold user
data). IR ships `bin/irmap.py` (drive `recv` to build a named JSON button map)
and `bin/irlookup.py` (identify live presses against `maps/`); run them from the
project root. They need `pip install pyserial`. `enable` also seeds `maps/` with a
library of remote button maps (NEC + Sony, incl. multi-position select switches)
from the `ir_maps` template dir via the spec's `seed_dirs` — existing maps are
never overwritten (the user's own maps win).

Host tools auto-detect the serial port the same way the monitor/upload scripts
do — by the board's USB VID/PID. `find_port.py` is both a CLI (`find_port.py
<board>`, used by the generated scripts) and a library: `find_for_project()`
reads the board from the nearest `cmdr.toml` `target` and returns the matching
port. `cmdr module enable` installs `find_port.py` into `bin/` alongside the
tools, so they pick the right board even when several USB serial devices are
attached (don't reintroduce "first `cu.usb*`" guessing). `cmdr module list` shows state per target.

## File layout (key files)

```
commander/
├── PLAN.md                      # roadmap and status — update as work lands
├── CLAUDE.md                    # this file
├── platformio.ini               # Arduino Uno/R4/Bluepill builds (run from repo root)
├── dev/<board>/                 # per-board dev scripts (build, bum, upload, monitor, bum-ota)
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
- Locomotion link (Phase R2): hardware-confirmed — a Pico 2 W master drives a real
  Roomba through the R4 I2C bridge. `modules/locomotion/` (Pico `LocomotionModule`
  `drive`/`stop`/`loco sensors` over `hal_i2c_*` ↔ R4 `loco-bridge` I2C slave →
  shared `Roomba`), plus the `i2c` scan/read/write diagnostic module.
- Pico W: builds clean, `help` confirmed over USB CDC serial.
- Pico 2 W (RP2350): builds clean via `dev/pico2/build`. Needs hardware test.
- ESP32-S3-N16R8: builds clean, `help` confirmed over native USB CDC (USB Serial/JTAG).
- STM32 Bluepill (STM32F103C8): hardware-confirmed — blink, `help` over USART1, `help`
  over USB CDC, and USB-DFU upload with no ST-Link. I2C/compass pending. `cmdr init
  bluepill <name>` scaffolds projects; `cmdr enable dfu` / `disable dfu` toggle the
  USB-DFU upload path (vs ST-Link only).

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
