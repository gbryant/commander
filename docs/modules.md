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
| `compass` | all but bluepill* | `heading`, `raw` | HMC5883L magnetometer over HAL I2C |
| `sonar` | all | `ping` | PING-style single-pin ultrasonic ranger |
| `i2c` | all but bluepill* | `i2c` | bus diagnostics: `scan` / `read` / `write` |
| `ina219` | all but bluepill* | `ina` | INA219 current/power monitor(s), multi-channel |
| `ds1302` | all | `rtc` | DS1302 RTC, bit-banged 3-wire |
| `wifi` | pico, pico2, r4, esp32 | `wifi` | `status` / `off` / `on` over runner hooks |
| `ir` | pico, pico2, uno, r4, unoq, esp32, bluepill | `ir recv` (+ `ir wall`, `ir diag`) | NEC/Sony IR receive |
| `roomba` | r4 | `oi` | iRobot Open Interface driver on `Serial1` |
| `locomotion` | pico, pico2 | `drive`, `stop`, `loco`, `bridge` | master side of the I2C mobile-base link |
| `loco-bridge` | r4 | `oi` | slave side: I2C → Roomba bridge |
| `controller` | pico, pico2 | `pad`, `bind`, `unbind`, `calibrate`, `btforget` | Bluetooth game-controller input |
| `serial_monitor` | pico, pico2 | `monitor` | stream a second UART into your session |
| `ipstube` | esp32 | `ipstube` | six ST7789 IPS displays (IPSTube clock) |
| `ws2812` | esp32, pico, pico2 | `wled` | addressable-RGB chain (RMT on esp32, PIO on pico) |
| `st7796` | pico, pico2† | `lcd` | ST7796S SPI TFT panel, 320×480 |
| `gt911` | pico, pico2, esp32, uno, r4 | `touch` | GT911 capacitive touch controller |
| `joystick` | pico, pico2† | `joy` | two-axis analog stick, calibrated + bindable |
| `buttons` | all | `btn` | debounced GPIO push buttons, bindable |
| `leds` | all | `led` | indicator LEDs: on/off/toggle/blink |
| `buzzer` | pico, pico2† | `buzz` | piezo buzzer: tones and note sequences |
| `aicam` | esp32 | `aicam` | Grove Vision AI V2 camera (SSCMA protocol) |

Boards also register a few commands outside the module system, from their runner:
`reset` (every runner except the Uno Q's, which registers none), `bootloader`
(Pico: USB bootloader; Bluepill: USB-DFU, with `cmdr enable dfu`), and `ota`
(R4/Pico/ESP32, with `cmdr enable ota`).

Two targets offer less than the table suggests, and `cmdr module list` reflects
that per project — the menu only shows what the HAL actually backs:

- **Uno Q** — only `system` and `ir`. Its Zephyr HAL backs the console/channel
  bus and IR so far; GPIO and I2C are stubbed.
- **Bluepill** (the `*` above) — everything except the I2C-backed modules
  (`compass`, `i2c`, `ina219`), since its STM32 I2C HAL is still stubbed.
- **SPI / ADC / PWM** (the `†` above) — `hal_spi_*`, `hal_adc_*` and `hal_pwm_*`
  are implemented in `hal/pico` only, so the modules that need them (`st7796`,
  `joystick`, `buzzer`) are offered on pico/pico2 alone. The other HALs carry
  honest stubs. Widen a module's platform list only when that platform's HAL
  grows the real thing — an enable-able module over a stubbed HAL is
  indistinguishable from broken hardware.

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

## Panel, touch and front-panel I/O

These came out of the Pico Breadboard Kit but none of them are kit-specific —
they're an ST7796 panel, a GT911 touch layer, an analog stick, buttons, LEDs and
a buzzer. The defaults match that kit's wiring because that's where they were
brought up; change the pins and they work anywhere.

> Not yet hardware-confirmed. The drivers are covered by host tests against the
> recording HAL in `tests/fakes/` — every byte they put on the wire is asserted —
> but they have not met the physical board. See PLAN.md.

### st7796 — SPI TFT panel
`lcd` — `info`, `on`/`off`, `bl <0-255>`, `clear`, `fill <colour>`,
`rect x y w h <colour>`, `text x y scale <text…>`, `line <n> <text…>` (a
full-width status row), `rotate 0-3`, `invert on|off`, `test` (colour bars,
border and text — the bring-up check).

Colours take a name (`red`, `cyan`, `dim`…) or a literal (`0xF800`, `63488`).
Text uses the built-in 5×7 font (`modules/display/Font5x7.h`) at any integer
scale.

Apps draw through **`IDisplay`** — deliberately the interface, not the driver, so
app screens survive a change of panel:

```cpp
extern "C" void commander_on_display_ready(IDisplay &d) {
    d.fill(color::kBlack);
    d.drawText(8, 8, "hello", color::kWhite, color::kBlack, 2);
}
```

`blit(x, y, w, h, const uint16_t *)` is shaped like LVGL's `flush_cb`, so
layering LVGL on top later is a binding, not a rewrite. There is no framebuffer —
320×480×2 is 300 KB — so drawing goes straight out the wire and the panel holds
the only copy. Questions: `sck`, `mosi`, `cs`, `dc`, `rst`, `bl` (−1 when the
backlight is hard-wired on), `rotation`. Which SPI controller the pins belong to
is derived from the SCK pin.

### gt911 — capacitive touch
`touch` — one reading; `info`, `raw`, `watch`/`stop`, `rotate 0-3`,
`flip x|y|xy|swap|none`.

Reports in **display** space, not panel space: `rotate` matches the display's
rotation and the flip flags handle a touch layer mounted differently from the
glass. `raw` shows the untransformed reading, which is what you want during
bring-up. Apps get points via `commander_on_touch_ready(Gt911Module&)` plus
`onTouch()` or `read()`.

This module is why the HAL grew `hal_i2c_write_read()`: the GT911 addresses
16-bit registers, which needs a combined write→read with a repeated start.

### joystick — two-axis analog stick
`joy` — position and direction; `raw`, `cal`, `deadzone <0-90>`, `watch`/`stop`,
`bind <dir> <command…>`, `unbind`, `binds`.

A generic input source, like `controller`: it publishes by poll (`x()`, `y()`,
`direction()`), by push (`onDirection`, `onButton`), and declaratively
(`joy bind up "lcd rotate 1"`). Axes are normalized to ±1000 around a centre
measured at init, with **two-sided scaling** so a stick that doesn't rest at
midscale still reaches full travel both ways. Diagonals resolve to the dominant
axis, so a menu gets one unambiguous direction. Calibration is spatial only —
temporal smoothing belongs at the consumer's loop rate, the same split
`ControllerCalibration` / `StickFilter` draws.

### buttons — debounced push buttons
`btn` — state and press counts; `watch`/`stop`, `bind <n> <command…>`,
`unbind`, `binds`. Up to 8 pins in one module, one command.

Debounced by **time in `tick()`, not by edge interrupts**: a bouncing contact
fires an ISR faster than a short guard window, and printing from inside an ISR
makes it worse. Polling at tick rate with a settle window can't re-enter.
Questions: `pins` (comma-separated), `active_low`, `debounce` ms.

### leds — indicator LEDs
`led` — all states; `led <n> on|off|toggle`, `led <n> blink <ms>`,
`led all on|off`. Blinking runs from `tick()`, so it never blocks the shell.
Questions: `pins`, `active_high`.

### buzzer — piezo buzzer
`buzz` — what's playing; `buzz <hz> [ms]`, `off`, `beep`,
`play <notes>`, `melody boot|ok|alert|fail`.

Note syntax is `<note><octave>[#|b]:<ms>` comma-separated, with `r` for a rest:
`buzz play c4:200,e4:200,g4:400`. Frequencies come from an integer table shifted
by octave — no libm, no floats. Everything is advanced from `tick()`, so a melody
never blocks the console. `BuzzerModule::noteToHz()` is public and useful on its
own (e.g. mapping a touch coordinate to a pitch).

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
`wled bright <0-255>`, `wled test`. Questions: `pin`, `count`, colour `order`. A
board's onboard RGB LED is just this with `count=1`. Apps drive effects via
`commander_on_ws2812_ready(...)`.

**Two backends, one module name and one command surface:** ESP32 drives the chain
from the RMT peripheral (`platform/esp32/Ws2812Module`), Pico from a PIO state
machine (`platform/pico/PicoWs2812Module`, bounded at 64 pixels, no heap on the
LED path). App code that drives LEDs moves between the two boards unchanged.

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
