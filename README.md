# commander

A portable embedded command shell. The same module code — compass, sonar, IR,
locomotion, displays — runs across an 8-bit Arduino Uno, the Raspberry Pi Pico W /
Pico 2 W, the ESP32-S3, the STM32 "Bluepill", and the dual-brain Arduino Uno Q.
Write a module once against a small C HAL; `cmdr` composes it into a project and it
builds for any target.

It's for **experimenting and prototyping** — one framework that scales from an AVR
up to a Linux-hosted board, without forking per platform.

![Nine development boards laid out in a row on a wooden surface, descending in
size from left to right: Arduino Uno, Arduino UNO R4 WiFi, Arduino UNO Q,
Raspberry Pi Pico, Pico W, Pico 2, Pico 2 W, an ESP32-S3 board, and an STM32
"Bluepill".](docs/img/boards.jpg)

*Left to right: Arduino Uno, UNO R4 WiFi, UNO Q, Raspberry Pi Pico, Pico W,
Pico 2, Pico 2 W, ESP32-S3, STM32 "Bluepill". The same module code runs on all
of them.*

## What you get

A shell on the board. Modules register commands; you talk to them over serial
or telnet while the firmware runs:

```console
> help
  help -- list all commands
  version -- firmware name, build number, commit
  rtc -- DS1302 real-time clock - 'rtc' / 'rtc set ...'
  ipstube -- 6x ST7789 displays - 'ipstube' for usage
  wifi -- WiFi status/control - 'wifi status|off|on'
  wled -- WS2812 LEDs - 'wled' for usage
  marquee -- scroll a message across all six displays
  reset -- reboot the firmware
  ota -- flash firmware from URL (http)
```

That's a real session on [cmdr-ipstube](docs/projects.md#cmdr-ipstube), a clock
built with it. `help`, `version`, `reset` and `ota` come from commander; `rtc`,
`ipstube`, `wifi` and `wled` are stock modules switched on with `cmdr module
enable`; `marquee` is the app's own. None of that wiring is hand-written.

## Why this and not …

**Arduino sketches** are the fastest way to start, and if one board and
`Serial.println` debugging cover your needs, stay there. Commander is for when
you want to *interrogate* a running device rather than re-flash it to change a
constant — and for when the same sensor code has to run on an AVR and a Pico
and an ESP32 without three forks of it.

**MicroPython / CircuitPython** give you a REPL, which is the closest thing to
this, and a faster edit loop. You trade away native SDK access, hard real-time
behaviour, and the small tiers — an Uno isn't a realistic target. Commander
keeps you in C++ on the vendor SDK and still gives you the interactive loop.

**Zephyr's shell** is genuinely portable and more capable than this one, but
it's Zephyr: a much larger commitment, and it won't run on an ATmega328.
Commander uses Zephyr as *one* backend (the Uno Q) rather than requiring it.

**ESPHome** is excellent at what it targets — declarative devices on ESP chips,
usually pointed at Home Assistant. Commander is imperative, C++, and
multi-vendor; different shape of problem.

**Your own serial command parser** is the honest comparison, because that's
what most people write. This is that, plus a HAL so modules move between
boards, plus a tool that composes them, plus transports (telnet, the Uno Q
channel bus) the parser would have grown eventually.

## Architecture in one sentence

```
core/  →  hal/<platform>/  →  modules/  →  transport/  →  platform/<board>/main.cpp
```

`core/` is pure C++ with zero platform deps. Modules include only `core/` and the C
HAL (`hal/hal.h`). Platform specifics are confined to `hal/`, `transport/`, and
`platform/`. One HAL `.cpp` per platform is compiled; the build excludes the rest.

## Supported boards

| Board | Build | Status |
|-------|-------|--------|
| Arduino Uno (AVR) | PlatformIO | ✅ shell over serial |
| Arduino R4 WiFi | PlatformIO | ✅ shell + WiFi + Telnet + mDNS (hardware-confirmed) |
| Raspberry Pi Pico W (RP2040) | CMake + Pico SDK | ✅ shell over USB CDC + WiFi + Telnet |
| Raspberry Pi Pico 2 W (RP2350) | CMake + Pico SDK | ✅ shell + WiFi + Telnet (hardware-confirmed); SMP/M33 |
| ESP32-S3 | ESP-IDF v5 | ✅ shell over native USB + WiFi + Telnet |
| STM32 Bluepill (F103) | PlatformIO (CMSIS) | ✅ shell over USART/USB CDC; USB-DFU upload (I2C pending) |
| Arduino Uno Q (Debian + M33) | Zephyr (west) + Debian services | ✅ shell + channel bus + IR (hardware-confirmed) — see the [IR-speaker walkthrough](docs/unoq-ir-speaker.md) |

## Quick start

```bash
# install the project manager
pip install "git+https://github.com/gbryant/commander.git#subdirectory=tools/cmdr"

# scaffold a project, add a module, build+upload+monitor
cmdr init pico myrobot
cd myrobot
cmdr module enable sonar
./bum
```

Full setup — host tools, SDK bootstrap, the env-var contract, and a per-board
prerequisite matrix — is in **[docs/getting-started.md](docs/getting-started.md)**.

## How it fits together

- **`cmdr`** (`tools/cmdr/`) scaffolds projects and composes modules. A project
  fetches commander as a CMake / PlatformIO dependency rather than vendoring it;
  `cmdr pull` adopts framework updates.
- **Modules** (`cmdr module enable <name>`) are composed, not hand-wired: enabling
  one records its config in `cmdr.toml` and regenerates the registration glue, so
  disabled modules aren't compiled. Cross-platform modules use the same code on
  every target; some are platform-gated (e.g. displays on ESP32, Bluetooth
  controllers on Pico).
- **The HAL is a C interface** (I2C, GPIO, UART, time). Porting to a new board is
  one HAL `.cpp` plus a `platform/<board>/` main.

## Documentation

- **[docs/getting-started.md](docs/getting-started.md)** — install, SDKs, first project.
- **[docs/modules.md](docs/modules.md)** — what every stock module does, per board.
- **[docs/writing-a-module.md](docs/writing-a-module.md)** — write your own module.
- **[docs/cmdr.md](docs/cmdr.md)** — the `cmdr` tool, command by command.
- **[docs/projects.md](docs/projects.md)** — real devices built with commander, as worked examples.
- **[CLAUDE.md](CLAUDE.md)** — deep per-board build details, modules, conventions.
- **[PLAN.md](PLAN.md)** — roadmap and status.
- **Arduino Uno Q** — [docs/unoq-ir-speaker.md](docs/unoq-ir-speaker.md) (start here: an
  end-to-end walkthrough, remote button → spoken name, no code),
  [docs/getting-started-unoq.md](docs/getting-started-unoq.md) (bring up a stock board),
  [docs/unoq-access.md](docs/unoq-access.md) (board access), and
  [dev/unoq/README.md](dev/unoq/README.md) (Debian-side bridge/broker). Generic
  board tooling (bring-up wizard, TTS, BT audio) lives in the companion
  [unoq-tools](https://github.com/gbryant/unoq-tools) repo.
- **Channel bus** (multi-consumer Linux-hosted commander) —
  [docs/commander-channels-design.md](docs/commander-channels-design.md).

## Project status

A **solo project**, built for my own hardware and shared because it may be
useful. It works — all seven boards are hardware-confirmed running the shell,
and the [projects](docs/projects.md) listed above are real devices in daily
use, not demos. But calibrate accordingly: there is one maintainer, and no
support promise.

- **Releases** are `vMAJOR.MINOR`; the major number moves only when a release
  breaks existing consumers. Scaffolded projects pin a release tag, so an
  upstream mistake can't reach a project generated last month.
- **Testing** is a local tiered suite — `tests/run.sh` (host C++ units + `cmdr`
  codegen golden files) and `tests/build-matrix.sh` (compile smoke across
  boards). GitHub Actions was deliberately declined for a solo project;
  see [docs/testing.md](docs/testing.md).
- **Known gaps** are tracked honestly rather than glossed: Bluepill I2C is
  stubbed (so the I2C modules aren't offered there), and pull-OTA is confirmed
  on the Pico W but not yet on the Pico 2 W. [PLAN.md](PLAN.md) marks state
  per area.
- **Issues and PRs** are welcome, but may be answered slowly. If you need a
  guaranteed response, fork it — that's what the licence is for.

## License

MIT — see [LICENSE](LICENSE). Embed it, ship it, sell it; just keep the notice.

Third-party code in this repository and the SDKs you supply at build time are listed
in [THIRD_PARTY.md](THIRD_PARTY.md), including the two dependencies with non-obvious
terms: BTstack (behind the optional `controller` module) and the optional STM32 DFU
bootloader.
