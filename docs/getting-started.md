# Getting started

How to go from a fresh machine to a board running commander. The build scripts do
most of the work — this is mostly about installing the host tools and SDKs they
expect, and the env-var contract that points them at the SDKs.

## Two audiences

- **Building your own project** (the common case): install `cmdr` (§4), the host
  tools + the *one* SDK for your target board (§1–2), then `cmdr init` a project
  that pulls commander as a dependency (§5). You don't clone or build this repo.
- **Hacking on commander itself:** same setup, but you also build the boards in
  `platform/` directly with `dev/<board>/...`, and you'll want every SDK (§2).

Either way the prerequisites are the same; only the last step differs.

## 1. Host tools

Install the tools your target board needs (full set shown; the per-board matrix in
§6 says which subset each board actually uses).

| Tool | Used by | macOS (Homebrew) | Debian/Ubuntu |
|------|---------|------------------|----------------|
| git, cmake, ninja, python3 | everything | `brew install cmake ninja python` | `apt install git cmake ninja-build python3` |
| **PlatformIO** | uno, r4, bluepill | `pipx install platformio` | `pipx install platformio` |
| arm-none-eabi-gcc | pico, pico2 (CMake) | `brew install --cask gcc-arm-embedded` | `apt install gcc-arm-none-eabi` |
| picotool | pico, pico2 | `brew install picotool` | build from source / `apt install picotool` |
| openocd | bluepill (ST-Link), unoq | `brew install openocd` | `apt install openocd` |
| dfu-util | bluepill (USB-DFU) | `brew install dfu-util` | `apt install dfu-util` |
| tio | esp32, bluepill (serial monitor) | `brew install tio` | `apt install tio` |
| adb | unoq (Debian access) | `brew install --cask android-platform-tools` | `apt install adb` |

ESP32 needs no separate compiler — ESP-IDF brings its own (§2). Some host-side
Python tools want extra packages on demand (`pip install pyserial` for the IR
tools, `pillow` for image tooling, `requests` for OTA upload) — each prints the
exact line when first run.

## 2. External SDKs

The vendor SDKs are too large to vendor into the repo, so they live on the host and
the build scripts locate them via env vars. `setup-sdks.sh` bootstraps the lot —
it lives in the commander repo, so grab a copy even if you're only *consuming* the
framework and never build this repo yourself:

```bash
git clone https://github.com/gbryant/commander.git ~/commander-src
~/commander-src/scripts/setup-sdks.sh
```

(Or run it from an existing checkout if you have one. Nothing else in §5 needs that
clone — it's just where the script is kept.)

It clones into `~/u-developer` by default (override with `COMMANDER_SDK_DIR=...`),
is idempotent (re-run any time — existing checkouts are skipped), and runs the
ESP-IDF `esp32s3` toolchain install. It clones the full set; you only need the ones
your board uses (§6), but cloning all of them is harmless.

What it installs: `pico-sdk`, `FreeRTOS-Kernel`, `pico_fota_bootloader` (Pico OTA),
`bluepad32` (Pico controller module), `stm32-dfu-bootloader` (Bluepill DFU),
`pngle` (ESP32 ipstube image tooling), and `esp-idf` (+ esp32s3 toolchain).

The two heavy toolchains are opt-out / opt-in so you don't clone a copy you won't
use:

```bash
scripts/setup-sdks.sh --no-esp-idf   # skip ESP-IDF + its toolchain
scripts/setup-sdks.sh --zephyr       # ALSO set up the Uno Q M33 Zephyr workspace
```

`--zephyr` creates a west workspace (`$COMMANDER_SDK_DIR/zephyrproject`, or set
`ZEPHYRPROJECT`) and reuses an existing ARM GNU toolchain (gnuarmemb) — no second
compiler copy. It needs `arm-none-eabi-gcc` available (macOS: `brew install --cask
gcc-arm-embedded`; Debian: `apt install gcc-arm-none-eabi`). This is the
prerequisite for **`cmdr init unoq`** projects — the Uno Q is a full target (builds
via west, flashes over the on-board OpenOCD). See
[getting-started-unoq.md](./getting-started-unoq.md).

## 3. Env-var contract

After cloning, point the scripts at the SDKs. If you used the default
`~/u-developer` location, **most of this is already the default** and you can skip
straight to the `esp` alias — set a var only if you keep that SDK elsewhere. Add to
`~/.zshrc` / `~/.bashrc`:

```bash
export PICO_SDK_PATH=~/u-developer/pico-sdk
export FREERTOS_KERNEL_PATH=~/u-developer/FreeRTOS-Kernel
export BLUEPAD32_PATH=~/u-developer/bluepad32
alias esp='. ~/u-developer/esp-idf/export.sh'   # load ESP-IDF for raw idf.py
```

| Var | Needed by | Default if unset |
|-----|-----------|------------------|
| `PICO_SDK_PATH` | pico, pico2 | — (required) |
| `FREERTOS_KERNEL_PATH` | pico, pico2, bluepill | — (required) |
| `BLUEPAD32_PATH` | pico `controller` module | — (required for that module) |
| ESP-IDF `export.sh` | esp32 | dev scripts self-source `~/u-developer/esp-idf/export.sh` (override via `IDF_EXPORT` / `IDF_PATH`) |
| `TINYUSB_PATH` | bluepill USB | `$PICO_SDK_PATH/lib/tinyusb` |
| `STM32_DFU_BOOTLOADER_PATH` | bluepill `cmdr enable dfu` | `~/u-developer/stm32-dfu-bootloader` |
| `ZEPHYRPROJECT` (or `ZEPHYR_BASE`/`ZEPHYR_VENV`) | unoq | `~/u-developer/zephyrproject` (falls back to `~/zephyrproject`) |

The esp32 `dev/esp32/*` scripts self-source ESP-IDF, so you don't need to run `esp`
first for a normal build — the alias is just for raw `idf.py`.

## 4. Install cmdr

`cmdr` is the project manager (scaffolds projects, composes modules, manages the
framework dependency). Install it from the repo:

```bash
pip install "git+https://github.com/gbryant/commander.git#subdirectory=tools/cmdr"
```

(or `pipx install "git+https://github.com/gbryant/commander.git#subdirectory=tools/cmdr"`).
Update later with `cmdr update`, which re-runs the same install against the latest
remote.

## 5. Your first project

```bash
cmdr init pico myrobot     # scaffold a Pico W project (board + name)
cd myrobot
cmdr module enable sonar   # answer the module's config questions
./bum                      # build + upload + monitor
```

`cmdr init <board> <name>` writes a project that fetches commander as a CMake/
PlatformIO dependency and generates its dev scripts (`bum`, `build`, `upload`,
`monitor`) at the project root. (On pico/pico2 the CMake configure step writes
them — if `PICO_SDK_PATH` wasn't set at init time, run the `cmake -B ...` line
that `cmdr init` prints once it is.)
`cmdr module enable <name>` records your answers in `cmdr.toml` and regenerates
`commander_modules.h`; `cmdr module list` shows what's available per target. Boards:
`uno`, `r4`, `pico`, `pico2`, `esp32`, `bluepill` (and `unoq`, see below).

### What a fresh project gives you

Straight after `cmdr init` — before enabling anything — a board answers `help`
with just the runner's own commands:

```console
> help
  reset -- reboot the firmware
  bootloader -- enter USB bootloader
  help -- list all commands
  version -- firmware name, build number, commit
> version
myrobot build 1 (2026-08-23 11:42)
```

That's the whole baseline: a working shell and nothing you didn't ask for. Each
`cmdr module enable` adds its commands to that list (and `cmdr enable ota` adds
`ota`), so `help` is always an accurate inventory of what this build actually
contains.

## 6. Per-board prerequisites

| Board | Build | SDKs (from §2) | Tools (beyond cmake/python) | Notes |
|-------|-------|----------------|------------------------------|-------|
| **uno** | PlatformIO | none | PlatformIO | pio pulls the AVR toolchain + FreeRTOS lib |
| **r4** | PlatformIO | none | PlatformIO | pio pulls the Renesas toolchain + Arduino_FreeRTOS |
| **bluepill** | PlatformIO | FreeRTOS-Kernel, TinyUSB, (stm32-dfu-bootloader for DFU) | openocd (ST-Link), dfu-util (USB), tio | pio provides the ARM compiler; flashing needs openocd or dfu-util |
| **pico / pico2** | CMake | pico-sdk, FreeRTOS-Kernel, (bluepad32 for controller, pico_fota_bootloader for OTA) | arm-none-eabi-gcc, ninja, picotool | pico2 overrides `PICO_BOARD=pico2_w` |
| **esp32** | ESP-IDF | esp-idf (brings its own toolchain) | tio | dev scripts self-source `export.sh` |
| **unoq** | Zephyr / west | zephyr workspace (`setup-sdks.sh --zephyr`) | arm-none-eabi-gcc, adb, openocd | Dual-brain (M33 firmware + Debian services); see below |

## Going deeper

- **Modules** — [modules.md](modules.md) is the reference for everything
  `cmdr module enable` offers; [writing-a-module.md](writing-a-module.md) shows
  how to build your own; [cmdr.md](cmdr.md) documents the tool itself.
- **Per-board build details** live in `CLAUDE.md` ("Building") — ports, flags, the
  patch scripts, OTA models.
- **Arduino Uno Q** is a dual-brain board (Debian + STM32U585 M33) and has its own
  setup track — start at `docs/getting-started-unoq.md`, then `docs/unoq-access.md`
  (board access) and `dev/unoq/README.md` (the Debian-side bridge/broker services).
  Generic board tooling (bring-up wizard, TTS, BT audio) is in the companion
  [unoq-tools](https://github.com/gbryant/unoq-tools) repo.
- **The channel bus** (multi-consumer Linux-hosted commander):
  `docs/commander-channels-design.md` and `docs/commander-channels-bringup.md`.
