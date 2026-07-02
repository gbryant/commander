# commander

A portable embedded command shell. The same module code — compass, sonar, IR,
locomotion, displays — runs across an 8-bit Arduino Uno, the Raspberry Pi Pico W /
Pico 2 W, the ESP32-S3, the STM32 "Bluepill", and the dual-brain Arduino Uno Q.
Write a module once against a small C HAL; `cmdr` composes it into a project and it
builds for any target.

It's for **experimenting and prototyping** — one framework that scales from an AVR
up to a Linux-hosted board, without forking per platform.

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
| Arduino Uno Q (Debian + M33) | M33 firmware + Debian services | ⚙️ channel bus hardware-proven; tutorial in progress |

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
- **[CLAUDE.md](CLAUDE.md)** — deep per-board build details, modules, conventions.
- **[PLAN.md](PLAN.md)** — roadmap and status.
- **Arduino Uno Q** — [docs/unoq-access.md](docs/unoq-access.md) (board access),
  [docs/unoq-linux-setup.md](docs/unoq-linux-setup.md), and
  [dev/unoq/README.md](dev/unoq/README.md) (Debian-side bridge/broker).
- **Channel bus** (multi-consumer Linux-hosted commander) —
  [docs/commander-channels-design.md](docs/commander-channels-design.md).
