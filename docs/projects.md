# Projects built with commander

The framework exists to be consumed. This is the catalogue of what's been built on
it — useful as worked examples (each one is a real, working device, not a demo),
and as the coordination list when commander itself moves or changes.

A consumer is an ordinary project that pulls commander in as a dependency and
provides two functions, `commander_config()` and `commander_setup()`. It does not
fork or vendor the framework. See [getting-started.md](getting-started.md) to make
one, and [modules.md](modules.md) for what each enabled module does.

**Naming.** Consumers use a `cmdr-` prefix. That's deliberate: it marks them as
commander projects without needing an umbrella folder or a monorepo — they sit as
siblings in `~/github/` and are found by their prefix.

## At a glance

| Project | Target | Build | Hardware | What it demonstrates |
|---------|--------|-------|----------|----------------------|
| [cmdr-ipstube](#cmdr-ipstube) | esp32 | CMake / ESP-IDF | IPSTube clock: classic ESP32, 6× ST7789, WS2812, DS1302 | Displays, filesystem assets, OTA — the most feature-complete consumer |
| [cmdr-ai-cam](#cmdr-ai-cam) | esp32 | CMake / ESP-IDF | XIAO ESP32-S3 + Grove Vision AI V2 + OV5647 | An MCU hosting an ML co-processor |
| [cmdr-robot](#cmdr-robot) | pico2 | CMake / Pico SDK | Pico 2 W + Bluetooth pad | Bluetooth input → I2C actuation; the master half of a two-board robot |
| [cmdr-oi-bridge](#cmdr-oi-bridge) | r4 | PlatformIO | Arduino Uno R4 WiFi + Roomba | The slave half; commander as an I2C peripheral |
| [cmdr-solar-monitor](#cmdr-solar-monitor) | esp32 | CMake / ESP-IDF | ESP32-S3-N16R8 + INA219 | The smallest useful consumer; firmware + host logging |
| [cmdr-unoq-ir-speaker](#cmdr-unoq-ir-speaker) | unoq | CMake / Zephyr (west) | Arduino Uno Q + IR receiver + BT speaker | Dual-brain: MCU real-time work feeding a Linux consumer. Zero custom firmware |
| [unoq-tools](#unoq-tools) | — | — | Arduino Uno Q (Debian side) | Not a consumer — a companion tooling repo |

Between them these cover all four build flavors (ESP-IDF, Pico SDK, PlatformIO,
Zephyr/west) and both link styles (`FetchContent` for CMake targets, `lib_deps`
for Arduino-framework ones).

---

## cmdr-ipstube

**A smart clock.** The IPSTube hardware is a classic ESP32 driving six ST7789
135×240 IPS displays, six WS2812 ambient LEDs, and a DS1302 RTC. Six displays on
one SPI bus with only three hardware CS lines is the interesting constraint — the
`ipstube` module solves it with a single `esp_lcd` panel and manual per-display
GPIO chip-select.

- **Modules:** `ipstube` (displays), `ws2812` (`wled`, GPIO5, 6 pixels GRB),
  `ds1302` (`rtc`, SCLK 22 / IO 19 / CE 21), `wifi`
- **Also enabled:** OTA (pull model), LittleFS
- **App-side:** font clock (`stb_truetype`), a PNG flip-clock face, NWS weather,
  newsapi headlines, a scene runner with marquee and vertical readers

This is the consumer that proved several framework features on real hardware:
displays and LEDs (2026-06-07), ESP32 pull-OTA via `bum-ota` (2026-06), and the
LittleFS partition composition. Its faces and fonts live on the filesystem rather
than in flash, so digit sets swap without reflashing.

Its display roadmap (module primitives vs. an app-side scene manager) is a live
design question — see the notes in PLAN.md.

## cmdr-ai-cam

*(Repo not yet published — still cooking; the `aicam` module it exercises ships in
commander today.)*

**An MCU hosting a vision co-processor.** A Seeed XIAO ESP32-S3 runs commander and
drives a Grove Vision AI Module V2 (WiseEye2 NPU) wearing an OV5647 camera. The
Vision AI runs the model; commander drives it, reads results back, and exposes the
whole thing as shell commands over USB serial or telnet.

- **Modules:** `aicam` (UART transport), `wifi`
- **Link:** UART on TX GPIO43 / RX GPIO44 @ 921600 (I2C at `0x62` is the
  alternative, selectable at enable time)

Hardware-confirmed 2026-06-11: `aicam info` / `sensors` plus live `aicam stream`
inference with a rock-paper-scissors model. Still unexercised on hardware: `snap`
(a 640×480 JPEG may exceed `AICAM_RX_MAX`) and the I2C transport.

The `aicam` module's value is the transport seam — `ISscmaTransport` /
`SscmaClient` is a clean-room port of the SSCMA framing with no Arduino, Wire, or
ArduinoJson dependency, so the protocol is portable and the wire is swappable.

Note the boundary: models are flashed with SenseCraft over the Vision AI's own
USB-C. Commander selects among flashed models and runs inference; it is not a
model-building tool.

## cmdr-robot

**The master half of a two-board robot.** A Pico 2 W takes Bluetooth game-controller
input and drives a Roomba through an I2C link to [cmdr-oi-bridge](#cmdr-oi-bridge).

- **Modules:** `controller` (Bluepad32/BTstack), `locomotion` (master, bridge at
  addr 66), `i2c` (SDA 6 / SCL 7), `wifi`
- **Control scheme:** left stick throttle, right stick X steering (arcs only),
  L2/R2 held for spin-in-place at slow/normal speed

The framework carries the reusable parts — `DriveMixer` for the ramped
throttle+steer → (velocity, radius) feel, `ControllerCalibration` for re-centering
sticks — and the project is just the glue that picks a stick layout. That split is
the point: the robot-specific decisions stay in the app.

Phase R2 is hardware-confirmed (2026-06-02): the Pico shell drove a real Roomba
through the bridge. Phase R3 rolled the Bluetooth backend into commander after a
standalone proof; **a full-stack re-test from the rolled-in `controller` module is
still pending** — the last open hardware item on this project.

It also runs WiFi and Bluetooth together on the one CYW43, which works because the
Pico runner owns a single shared `cyw43_arch_init()`.

## cmdr-oi-bridge

**Commander as an I2C peripheral.** An Arduino Uno R4 WiFi sits between the Pico
master and a Roomba, forwarding `CMD_LOCO_*` commands to the Open Interface over
`Serial1` and serving back a sensor snapshot.

- **Modules:** `loco-bridge` (I2C slave at addr 66 on `Wire1`/Qwiic, 3.3 V; BRC
  wake on D4; 115200 baud), `wifi`
- **Was:** `r4-test`

Two design details worth reading the code for. The Wire `onReceive`/`onRequest`
handlers run in ISR context, so they only latch the command and serve a pre-cached
snapshot — the blocking Roomba UART I/O happens in `tick()`. And the bridge reads
the base **lazily**, only when the master asks: a free-running poll both stuttered
the drive stream and kept the Roomba awake forever, draining its battery.

The master can also reach the bridge's own shell remotely (`bridge <cmd>`,
`bridge reset`), which means you can triage or reboot the R4 from the Pico's
console even when the R4's WiFi is unreachable.

Watch the RAM budget here — the R4 has 32 KB SRAM, and OTA + IR + `loco-bridge` +
WiFi together overflow it.

## cmdr-solar-monitor

**The smallest useful consumer.** An ESP32-S3-N16R8 reads a solar panel through an
INA219 and serves the numbers over WiFi/telnet; host scripts poll it into SQLite
and graph the result.

- **Modules:** `ina219` (channel `a:0x40`, 0.1 Ω shunt), `i2c` (SDA 8 / SCL 9)
- **Host side:** `scripts/poll_solar.py` → `solar.db`, `scripts/graph_solar.py`

Worth pointing at when someone asks what a minimal commander project looks like:
two modules, no custom firmware logic to speak of, and the interesting work is a
host script talking to `ina stats` over telnet. It's also the shape most sensor
projects want — the device serves readings, the host does the storage and
presentation.

No hardware-confirmation date is recorded for this one in PLAN.md.

---

## cmdr-unoq-ir-speaker

**Point a remote at it and it says the button's name.** An Arduino Uno Q: the
STM32U585 (M33, Zephyr) decodes IR pulse trains and publishes each press on
channel 1, while a Python subscriber on the QRB2210's Debian side matches it
against `maps/` and speaks the name through a warm Piper voice on a Bluetooth
speaker. The `ch0` console stays free the whole time.

- **Modules:** `ir` (D5, NEC + Sony)
- **Autostart:** `ir recv` — receiving is a standing board capability, so a fresh
  boot streams presses with no command sent
- **SBC side:** `bin/ir_speak.py` subscribing to the broker's `ch1.sock`, with
  `ir_map.py` / `ir_lookup.py` and a seeded library of remote maps
- **Board side:** TTS + Bluetooth from the companion
  [unoq-tools](https://github.com/gbryant/unoq-tools)

**`src/main.cpp` is the stock 13-line template — the emptiness is the point.**
The whole device is composition: `cmdr init unoq`, `cmdr module enable ir`,
`cmdr autostart add "ir recv"`. It's the clearest demonstration of what the Uno Q
target is for — hard real-time on the MCU, something an MCU can't do (neural TTS)
next to it, and the channel bus between them carrying data that neither end's
transport knows anything about.

It also **runs standalone**: `./deploy-sbc --service "ir_speak.py --greeting"`
plus `bt.py autoconnect on <MAC>` make it an appliance that boots on a wall
socket, announces itself, and needs no computer.

Hardware-confirmed 2026-08-12: 18 presses decoded, matched and spoken, and the
full chain coming up by itself after a reboot. The walkthrough that builds this
device from nothing is [unoq-ir-speaker.md](unoq-ir-speaker.md).

## unoq-tools

Not a commander consumer — a **companion repo** of host-side tooling for the
Arduino Uno Q's Debian side, split out of commander on 2026-08-04.

Everything runs from your computer over adb (no keyboard or monitor on the board),
and the wizards are idempotent: they show current state and ask before changing
anything. Contents: `setup-board.py` (first-boot wizard), Piper TTS (`tts.py`,
`tts_daemon.py`, `tts-bench.py`, a systemd unit), Bluetooth audio (`bt.py`,
`setup-bt-audio.py`), `espeak.py`, `volume.py`, and `docs/` covering headless Linux
trimming, BT audio, factory restore, and the on-board ML backend.

It lives outside commander because it's board-generic: it doesn't know or care what
firmware the M33 is running. Commander's Uno Q track uses it for bring-up and voice
output, and `docs/getting-started-unoq.md` plus `scripts/setup-sdks.sh` point users
at it. The Uno Q firmware side stays in commander (`hal/zephyr/`, `runners/zephyr/`,
`platform/zephyr/`, `dev/unoq/` for the bridge and broker).

---

## Scratch projects

Some scaffolds are kept as local testing projects. They're deliberately **not**
under source control and aren't part of this catalogue:

- `cmdr-q-latest`, `cmdr-unoq-test` — Uno Q scaffolds from `cmdr init unoq`.
- `cmdr-pico-ota-test` (Pico W), `cmdr-pico2-ota-test` (Pico 2 W) — the
  hardware-in-the-loop OTA fixtures. Re-run them after changing the runner's OTA
  path, `ota_push.py`, or the `pico_fota_bootloader` wiring; the procedure and its
  traps are in [testing.md](testing.md) under Tier 4. Both can be recreated from
  scratch with `cmdr init` + `cmdr enable ota` if lost.

## Coordination

Consumers reference commander **by URL** — `FetchContent` for the CMake targets,
`lib_deps` for the PlatformIO ones. So moving or renaming commander breaks every
consumer's build until each is repointed — treat the list above as the checklist
for any such move.

Version pinning is per-project: `cmdr pin <ref>` / `--latest` / `cmdr unpin` lock
or float a consumer's commander version, and `cmdr link <path>` builds against a
local checkout for framework development. Consumers pin a release tag, so
adopting framework changes is two deliberate steps — `cmdr pin <tag>` then `cmdr pull` —
always against published commander, never a local override.
