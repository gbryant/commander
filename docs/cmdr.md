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
`.gitignore` (covers `secrets.h`, build output and the dev scripts), `secrets.h`
on WiFi targets, and the dev scripts `bum` / `build` / `upload` / `monitor`
(+ `bum-ota` where supported) at the project root. `./bum` = build + upload +
monitor. The scripts are generated rather than committed — see
[Maintenance](#maintenance).

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

A project fetches commander from GitHub at build time.

**These four are for CMake projects** (`pico`, `pico2`, `esp32`, `unoq`), where the
version is a `GIT_TAG` in `CMakeLists.txt`. **PlatformIO projects** (`uno`, `r4`,
`bluepill`) pin through the `lib_deps` git ref in `platformio.ini` instead — append
a tag (`…/commander.git#v1.1`) to pin, drop it to float — and the commands below
will tell you so rather than running.

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

**Moving to a newer release is a separate, deliberate step.** A scaffold pins a
release tag, so `cmdr pull` on its own re-fetches *that same tag* and changes
nothing. To take a new release on a CMake project: `cmdr pin v1.2` (or `--latest`
to freeze main's current tip, or `cmdr unpin` to track `main`), then `cmdr pull`.
On a PlatformIO project, bump the `#tag` on the `lib_deps` ref and rebuild. Either
way it's the point of pinning — updates arrive when you ask.

### When cmdr is newer than your framework

cmdr installs from `main`; projects pin a release. So cmdr can generate code that
calls framework APIs your pinned version doesn't have. Rather than let that
surface as a compile error inside `commander_modules.h`, cmdr checks the pin
before generating and offers both fixes:

```
This cmdr generates code for commander >= v2.0, but this project pins v1.1.

  Upgrade the project:  cmdr pin v2.0 && cmdr pull
  Or match the project: pip install --force-reinstall \
      "git+https://github.com/gbryant/commander.git@v1.1#subdirectory=tools/cmdr"
```

**Staying on an older framework is a supported state** — install the cmdr that
shipped with it. The check reads the pin from wherever your build does
(`GIT_TAG` in CMakeLists.txt, or the `#tag` on the `lib_deps` ref in
platformio.ini); it is skipped while `cmdr link` is active, and ignores pins it
cannot compare, such as a branch name or a bare commit.

## Maintenance

A project has four layers with three owners — see
[cmdr-regen.md](cmdr-regen.md) for the model:

| Command | Refreshes |
|---------|-----------|
| `cmdr pull` | the fetched framework dependency, **at whatever `GIT_TAG` the project pins** (+ reconfigure) |
| `cmdr clean` | build artifacts: build dirs, `.pio/`, fetched `_deps`, esp32 `sdkconfig` |
| `cmdr regen [--dry-run]` | cmdr-generated files: dev scripts, `commander_modules.h`, module `bin/` tools |

The dev scripts are **generated, not source** — a scaffolded project gitignores
them, so a fresh clone starts without any. `cmdr regen` writes them, and is also
how an existing project picks up template fixes. (On pico/pico2 they come from the
CMake configure step instead, via `commander_generate_scripts()`.)

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
serial = "5&1D2B3C4&0&2"            # preferred: identifies the board itself
port   = "/dev/cu.usbserial-1430"   # when the chip reports no serial (CH340s don't)
```

Prefer `serial`: it identifies the physical board, so it keeps working after a
re-plug, on a different USB port, or on another machine. Many cheap USB-serial
chips (CH340s among them) report no serial at all, and then the device path is
the only stable handle you have. Existing projects pick this up with `cmdr regen`.

