# cmdr reference

`cmdr` is the project manager: it scaffolds projects that fetch commander as a
dependency, composes [modules](modules.md), and manages the framework version.
Install and update:

```bash
pip install "git+https://github.com/gbryant/commander.git#subdirectory=tools/cmdr"
cmdr update          # later: reinstall from the latest remote
```

All commands except `init`, `update`, and `config` run from a project root.

## Projects

### `cmdr init <target> <name>`

Scaffolds a project directory. Targets: `pico`, `pico2`, `esp32`, `uno`, `r4`,
`bluepill`, `unoq`. ESP32 takes memory options: `--chip` (default `esp32s3`),
`--flash` MB, `--psram` MB.

What you get: a hook `main.cpp` (`commander_config()` + `commander_setup()`),
`cmdr.toml` (the module manifest), the generated `commander_modules.h`, a
`.gitignore` (covers `secrets.h` and build output), `secrets.h` on WiFi targets,
and the dev scripts `bum` / `build` / `upload` / `monitor` (+ `bum-ota` where
supported) at the project root. `./bum` = build + upload + monitor.

On pico/pico2 the scripts are written by the CMake configure step, which `init`
runs if `PICO_SDK_PATH` and `FREERTOS_KERNEL_PATH` are set (otherwise it prints
the configure line to run once they are). See
[getting-started.md](getting-started.md) for the SDK setup.

### `cmdr config wifi <ssid> <password>`

Stores default WiFi credentials in `~/.cmdr/config`; subsequent `init`s
pre-fill `secrets.h`.

## Modules

### `cmdr module enable <name>` / `disable <name>` / `list`

`enable` asks the module's config questions (pins, addresses, options — with
per-target defaults), records them in `cmdr.toml`, and regenerates
`commander_modules.h` so only enabled modules compile. It also installs any
companion host tools into `bin/`, seeds data dirs, adds PlatformIO `lib_deps`,
and injects build flags — whatever the module declares. `disable` reverses all
of that but preserves data dirs (your data). `list` shows every module's state
for your target. `MAX_COMMANDS` is auto-sized to the enabled set.

### `cmdr autostart add "<cmdline>"` / `remove` / `list` / `clear`

Boot commands, recorded in `cmdr.toml` `[autostart]` and dispatched once at
startup (output discarded — you want the side effect). Example:
`cmdr autostart add "ir recv"` makes a fresh board stream IR presses with no
app code.

## Features

### `cmdr enable ota` / `disable ota`

Over-the-air updates; the model differs by board:

- **Pico / ESP32 (pull):** the runner registers `ota <url>` — the board
  downloads firmware and self-flashes. The generated `bum-ota` script serves
  the build over HTTP and sends the command.
- **R4 (push):** on-demand — the `ota start` command hands the socket pool from
  Telnet to ArduinoOTA; `bum-ota` arms it over Telnet then pushes.

`version` reports the build name/number stamped at build time, so `bum-ota`
can confirm the new image landed.

### `cmdr enable littlefs [--size MB] [--label L] [--dir D]` (ESP32)

Adds a LittleFS data partition and a build step that images `--dir` into it.
The partition table is *composed*: OTA and LittleFS stack in either order, and
disabling one preserves the other. Mount from the app with
`commander_mount_littlefs(label, base)`.

### `cmdr enable dfu` (Bluepill)

Switches to USB-DFU uploads (no ST-Link needed after a one-time bootloader
flash via `./flash-bluepill-bootloader`): links the app above the bootloader,
adds the `bootloader` shell command, and rewires `./bum` to reboot the board
into DFU and flash with `dfu-util` (`./upload` stays ST-Link).

## Framework version

A project fetches commander from GitHub at build time. Four commands control
which version:

| Command | Effect |
|---------|--------|
| `cmdr pin <ref>` | lock the project to a commit/tag/branch (`--latest` = freeze current main tip; bare `pin` shows state) |
| `cmdr unpin` | float back to `main` (deliberately track the tip; new projects start pinned to a release tag) |
| `cmdr link <path>` | build against a local commander checkout (framework development; bare `link` shows status) |
| `cmdr unlink` | back to GitHub |

Releases are tagged **`vMAJOR.MINOR`**. The major number moves **only** when a
release breaks existing consumers; everything else bumps the minor. So moving
`v1.2 → v1.4` is safe by contract, and a jump to `v2.0` is the one to read the
release notes for.

## Maintenance

A project has four layers with three owners — see
[cmdr-regen.md](cmdr-regen.md) for the model:

| Command | Refreshes |
|---------|-----------|
| `cmdr pull` | the fetched framework dependency, **at whatever `GIT_TAG` the project pins** (+ reconfigure) |
| `cmdr clean` | build artifacts: build dirs, `.pio/`, fetched `_deps`, esp32 `sdkconfig` |
| `cmdr regen [--dry-run]` | cmdr-generated files: dev scripts, `commander_modules.h`, module `bin/` tools |

`regen` never touches hand-written source, `cmdr.toml`, or
`CMakeLists.txt`/`platformio.ini` — your files are yours.

## Picking the right board when several are plugged in

The dev scripts and host tools find your board by USB VID/PID (`find_port.py`,
installed into the project). That's exact when each board has a distinct chip —
but **two boards can share one**: plug in two CH340-based boards and they are
genuinely indistinguishable, as are an ESP32 devkit and a CH340 Uno clone.

Rather than guess (and silently connect you to the wrong board, which looks
exactly like a dead one), the scripts stop and list the candidates. Resolve it
per-run or per-project:

```bash
CMDR_PORT=/dev/cu.usbserial-1430 ./monitor    # one run
```

```toml
# cmdr.toml — persistent
serial = "0001"                     # preferred: identifies the board itself
port   = "/dev/cu.usbserial-1430"   # when the chip reports no serial (CH340s don't)
```

`serial` survives re-plugging and differs per machine-independent board, so
prefer it; many cheap USB-serial chips report none, and then the path is the
only stable handle. Existing projects pick this up with `cmdr regen`.

**Moving to a newer framework release is a separate, deliberate step.** A scaffold pins a
release tag, so `cmdr pull` on its own re-fetches *that same tag* and changes nothing. To take a
new release: `cmdr pin v1.1` (or `--latest` to freeze main's current tip, or `cmdr unpin` to
track `main`), then `cmdr pull`. That's the point of pinning — updates arrive when you ask.
