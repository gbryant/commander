# Module reference

Modules are commander's unit of functionality: each one registers shell commands
and (optionally) exposes a C++ API to your app through a weak hook. You compose
them with the [`cmdr` tool](cmdr.md) — `cmdr module enable <name>` asks the
module's config questions, records the answers in `cmdr.toml`, and regenerates
`commander_modules.h` so that **only enabled modules are compiled**. To write
your own, see [writing-a-module.md](writing-a-module.md).

Run `cmdr module list` inside a project to see what's available for your target.

## At a glance

| Module | Platforms | Commands | What it is |
|--------|-----------|----------|------------|
| `system` | all (always on) | `help`, `version` | command list + build identification |
| `compass` | all | `heading`, `raw` | HMC5883L magnetometer over HAL I2C |
| `sonar` | all | `ping` | PING-style single-pin ultrasonic ranger |
| `i2c` | all | `i2c` | bus diagnostics: `scan` / `read` / `write` |
| `ina219` | all | `ina` | INA219 current/power monitor(s), multi-channel |
| `ds1302` | all | `rtc` | DS1302 RTC, bit-banged 3-wire |
| `wifi` | pico, pico2, r4, esp32 | `wifi` | `status` / `off` / `on` over runner hooks |
| `ir` | pico, pico2, uno, r4, unoq, esp32, bluepill | `ir recv` (+ `ir wall`, `ir diag`) | NEC/Sony IR receive |
| `roomba` | r4 | `oi` | iRobot Open Interface driver on `Serial1` |
| `locomotion` | pico, pico2 | `drive`, `stop`, `loco`, `bridge` | master side of the I2C mobile-base link |
| `loco-bridge` | r4 | `oi` | slave side: I2C → Roomba bridge |
| `controller` | pico, pico2 | `pad`, `bind`, `unbind`, `calibrate`, `btforget` | Bluetooth game-controller input |
| `serial_monitor` | pico, pico2 | `monitor` | stream a second UART into your session |
| `ipstube` | esp32 | `ipstube` | six ST7789 IPS displays (IPSTube clock) |
| `ws2812` | esp32 | `wled` | addressable-RGB chain over RMT |
| `aicam` | esp32 | `aicam` | Grove Vision AI V2 camera (SSCMA protocol) |

Boards also register a few commands outside the module system, from their runner:
`reset` (everywhere), `bootloader` (Pico: USB bootloader; Bluepill: USB-DFU, with
`cmdr enable dfu`), and `ota` (R4/Pico/ESP32, with `cmdr enable ota`).

On the **Uno Q** only `system` and `ir` are offered — its Zephyr HAL backs the
console/channel bus and IR so far; the menu stays honest about what works.

## Sensors and buses

### compass — HMC5883L magnetometer
`heading` prints 0–359°; `raw` prints X/Y/Z counts. Questions: `sda`/`scl`
(defaults: GP6/GP7 on Pico, GPIO4/5 on ESP32, A4/A5 on Uno/R4). Enabling any
I2C module brings up the shared HAL bus once — identical `hal_i2c_init` lines
are deduped across modules.

### sonar — ultrasonic ranger
`ping` measures distance (cm/in). One signal pin (trigger + echo on the same
pin, RadioShack 276-0342 / Parallax PING style). Question: `pin` (default 6).

### i2c — bus diagnostics
`i2c scan` walks the bus, `i2c read <addr> <reg> <n>` and
`i2c write <addr> <bytes…>` poke devices directly. Handy for bringing up new
I2C hardware (it's how the locomotion bridge was debugged).

### ina219 — current/power monitor
One `ina` command for any number of sensors: the `channels` question is a comma
list of `label:addr` (e.g. `a:0x40,b:0x45`). `ina` lists channels;
`ina <ch> volt|amp|watt|stats|init` reads one; `ina stats` dumps CSV for all.
Calibrated for the common 0.1 Ω shunt breakout.

### ds1302 — real-time clock
`rtc` prints the time, `rtc set YYYY-MM-DD HH:MM:SS` sets it, `rtc dump` shows
raw registers. Bit-banged 3-wire over `hal_gpio_*`, so it works on every
platform. Questions: `sclk`/`io`/`ce` pins. Apps read/write time via the
`commander_on_ds1302_ready` hook.

## Connectivity

### wifi — WiFi status/control
`wifi status`, `wifi off` (also suppresses auto-reconnect), `wifi on`. The
command is portable; each runner implements the underlying hooks
(`commander_wifi_status/off/on` in `core/WifiHooks.h`).

### serial_monitor — UART tap
`monitor` streams whatever arrives on a second hardware UART into your telnet
or serial session — monitor another board without a second USB cable.
Questions: `uart` instance, `rx`/`tx` pins, `baud` (defaults: UART1, GP9/GP8,
115200).

## IR

### ir — NEC/Sony receive
`ir recv` toggles receive mode; decoded presses stream to the console. Each
platform has a native backend (Uno/R4: IRremote; Pico: PIO on core 1, adds
`ir diag`; ESP32: RMT; Bluepill: EXTI/DWT; Uno Q: Zephyr GPIO → channel 1).
Default pins: 5 (Uno/R4/Uno Q), GP22 (Pico), GPIO38 (ESP32), `0x10` = PB0
(Bluepill).

Opting in to the `wall` feature at enable time adds `ir wall` (Roomba virtual
wall detection); it's off by default so it costs no flash, RAM, or command slot.

Enabling `ir` also installs host tools into your project's `bin/` —
`irmap.py` (build a named JSON button map) and `irlookup.py` (identify live
presses) — and seeds `maps/` with a library of known remotes. They need
`pip install pyserial` and auto-detect the board's serial port by USB VID/PID.

## Robot stack

The locomotion pair lets a Pico drive a Roomba through an R4 acting as an I2C
bridge. The wire format lives in `modules/locomotion/LocoProtocol.h` and the
register IDs in `include/i2c_ids.h` — shared by both sides.

### roomba — direct OI driver (R4)
`oi` exposes drive/clean/dock/sensor commands straight over `Serial1` (D0/D1).
Questions: `baud`, optional `brc` wake pin. Mutually exclusive with
`loco-bridge`, which owns `Serial1` and provides `oi` itself.

### locomotion — master side (Pico/Pico 2)
`drive forward|back|left|right [speed]` (or raw `drive <vel> <radius>`) and
`stop` command the bridge; `loco sensors` requests a fresh sensor snapshot. `bridge <cmd>` is a remote console — it runs any command
on the bridge board's own shell and streams the output back; `bridge reset`
hard-resets the R4. For analog driving, `modules/locomotion/DriveMixer.h` turns
a normalized stick pair into smooth ramped drive commands.

### loco-bridge — slave side (R4)
The matching I2C slave: forwards `CMD_LOCO_*` to the Roomba, serves cached
sensor snapshots (ISR-safe), parks the base after idling to save its battery,
and optionally pulses a BRC line to wake it. Questions include the I2C `port`:
`Wire1` = the Qwiic connector (3.3 V, wire a Pico straight in — the default) or
`Wire` = A4/A5 (5 V, needs a level shifter).

### controller — Bluetooth gamepad (Pico/Pico 2)
A generic input source (Bluepad32 + BTstack), not robot-specific. Three ways to
consume it: poll `state()`, push callbacks (`onUpdate`/`onButton`), or the
declarative `bind <button> <command…>` (dispatch any shell command on press).
`pad` shows status + bindings; `calibrate` runs an interactive 4-phase stick
calibration; `btforget` clears stale BT bonds so a pad can re-pair. Sticks are
re-centered/rescaled before publishing; apps needing temporal smoothing apply
`StickFilter` at their own loop rate. Enabling it is heavy (BT firmware) and
needs `BLUEPAD32_PATH`; WiFi and BT share the one CYW43 radio, so telnet keeps
working.

## Displays and LEDs (ESP32)

### ipstube — six ST7789 displays
The driver for the IPSTube clock's 135×240 panels: one shared SPI bus,
per-display chip-select. `ipstube on/off/dim/fill/clear/test`, text rendering
(`text`/`fit`/`wrap`/`flow`/`scroll`), and panel-debug subcommands
(`cs`/`reinit`/`invert`/`swap`/`mirror`/`gap`); apps draw via
`commander_on_ipstube_ready(IpstubeModule&)`
(`drawBitmap`/`fill`/`backlight`). All pins and panel tunables are
`-DIPSTUBE_*` overridable. Ships `bin/img2rgb565.py` for image conversion.

### ws2812 — addressable RGB
`wled <r> <g> <b>` (all), `wled <i> <r> <g> <b>` (one), `wled off`,
`wled bright <0-255>`. Questions: `pin`, `count`, colour `order`. A board's
onboard RGB LED is just this with `count=1`. Apps drive effects via
`commander_on_ws2812_ready(Ws2812Module&)`.

## Vision (ESP32)

### aicam — Grove Vision AI Module V2
Runs inference on the Vision AI's flashed models and streams results.
`aicam info/model/models/sensor/score/iou/invoke/stream on|off/snap`, plus
`aicam at <raw>` as an AT passthrough. The `transport` question picks UART
(better image throughput) or I2C (frees both USB-C ports). Apps consume results
via `commander_on_aicam_ready(AiCamModule&)` + `onResult`. Flashing models
stays a SenseCraft job over the camera's own USB-C.

## Autostart

Any command can run at boot: `cmdr autostart add "ir recv"` records it in
`cmdr.toml`, and the generated code dispatches it after the ready-hooks (so
output routing is already wired). `list` / `remove` / `clear` manage the set.
Stopping a started stream is the command's own toggle. This is how a fresh
board can stream IR with zero app code.
