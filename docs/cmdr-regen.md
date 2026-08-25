# Design note: project-file lifecycle — `regen` / `pull` / `clean`, and the shim path forward

**Status:** `cmdr regen` + the `install-broker` shim IMPLEMENTED 2026-06-17. This note records the
mental model the three maintenance commands share and the long-term direction (thin shims) that
shrinks `regen`'s job over time.

## The problem

`cmdr` scaffolds a project once at `init`, then the framework evolves. Generated files in the
project (dev scripts, `commander_modules.h`) **freeze at init** — `cmdr pull`/`update` refresh the
framework and the tool, but never rewrite scaffolded scripts. So a framework fix (e.g. the
`install-broker` one-sudo fix, the esp32 IDF self-sourcing, the ch1-only IR tools) doesn't reach
existing projects, and you hand-edit each one. `cmdr regen` closes that gap.

## The mental model: 4 layers, 3 commands

A project has four kinds of content; three maintenance commands each own one non-source layer:

| Layer | Examples | Command | Effect |
|-------|----------|---------|--------|
| Hand-written source | `main.cpp`, `cmdr.toml`, `secrets.h`, `maps/` | — | never auto-touched |
| Generated project files | dev scripts, `commander_modules.h`, `bin/` tools | **`cmdr regen`** | re-emit from current templates |
| Fetched framework dep | `build-*/_deps/commander-src`, `.pio/libdeps/commander` | **`cmdr pull`** | re-fetch at the pinned `GIT_TAG` + reconfigure (keeps build cache). A *newer release* needs `cmdr pin` first |
| Build artifacts | build dirs, `.pio/`, `sdkconfig`, caches | **`cmdr clean`** | wipe (next build re-fetches) |

They're complementary, not redundant. The one real overlap — `pull` and `clean` both cause a
re-fetch — is **intentional granularity**: `pull` is surgical (refresh the dep, keep the rest of the
build cache → fast), `clean` is the full wipe.

## `cmdr regen`

Re-emits the generated layer from current templates, reading `target` + modules from `cmdr.toml`:
- per-target dev scripts + `scripts/` helpers (shared with `init` via `_emit_scripts`, so the two
  can't drift), `commander_modules.h` (from the manifest), and enabled modules' `bin/` host tools.
- recovers init-only params from the existing project (project name from the PlatformIO env or the
  dir name; esp32 chip from the existing build script's `set-target`).
- `--dry-run` lists changes without writing.

**Excludes — by design:**
- Hand-written source, `cmdr.toml`, user data (`maps/`).
- **`CMakeLists.txt` / `platformio.ini`.** These are init-seeded but then **accumulate state**
  (`enable ota/dfu/littlefs`, `link`, `pin`, `lib_deps`, `MAX_COMMANDS`). Re-emitting the template
  would wipe that. Template-level changes to *these* files are **targeted migrations**, not regen —
  e.g. the `FetchContent_Populate` → `MakeAvailable` fix, which had to be applied surgically.
- **Pico/pico2** emit no dev scripts here: theirs come from CMake's `commander_generate_scripts` at
  configure time, so they refresh on `cmdr pull` / reconfigure. `regen` on pico does
  `commander_modules.h` + tools and prints a note.

## Why not generate all scripts at configure time (like pico)?

Pico's `commander_generate_scripts(TARGET)` (`cmake/GenerateScripts.cmake`) `configure_file`s the
dev scripts at `cmake` configure, from the fetched framework — so they're never frozen. Tempting to
do everywhere, but it doesn't generalize:

1. **Two build-system families.** pico/esp32/unoq are CMake; uno/r4/bluepill are PlatformIO (no
   CMake) and would need a different vehicle (a SCons `extra_scripts` hook). Not one mechanism.
2. **Bootstrapping / init would need the toolchain.** The dev scripts *wrap* the build tool, yet
   they'd be generated *by* configuring. Pico gets away with it because `init` runs a cheap
   `cmake -B`. esp32/unoq init deliberately *don't* configure (they need the IDF/Zephyr env), and
   forcing it would make `cmdr init` require the full toolchain up front — losing the "scaffold a
   complete project with no toolchain, build later" property.
3. **Not all scripts are build steps.** `install-broker`, `enable-flash-boot`, `restore-arduino`,
   `deploy-sbc` are board-management, run independent of a build — configure-time generation is the
   wrong fit (and that's exactly where the staleness bug lived).
4. ~~**Committed-vs-generated model shift.**~~ *(No longer applies — 2026-08.)* This argued that
   cmdr commits scripts at init while configure-generation pushes toward
   gitignored/regenerated-locally. The tool has since moved to the latter for every target:
   `PROJECT_GITIGNORE` excludes the generated scripts, and `cmdr regen` is how a project gets
   them back. The other three objections stand.

## The north star: thin shims (and why `regen` is still the floor)

The deeper fix is to stop putting logic in the project at all — make each script a **thin shim that
delegates to the fetched framework**, so the *logic* lives in commander (refreshed by `pull`/`clean`)
and the project stub only changes if its *interface* changes (rare).

**First conversion (done): `install-broker`.** The logic moved to
`dev/unoq/install_broker.sh` (shipped with commander, fetched into `build-unoq/_deps`); the project's
`install-broker` is now a stub that `find`s and `exec`s it. It no longer drifts — a fix to the broker
install logic reaches existing projects via `clean`/`build`, no regen needed.

**What can't be a shim (so `regen` doesn't go away):**
- `build` / `bum` — `build` *triggers* the fetch, so before the first build there's nothing to
  delegate to. Its bootstrap logic (env-sourcing, `set-target`) is irreducibly project-local.
- PlatformIO `extra_scripts` (`stm32_build.py`, `version_stamp.py`, `patch_freertos.py`) — run
  *during* the build but are needed *to* build, before the lib is installed.
- `commander_modules.h` — generated C++ the firmware `#include`s; must be a real file.

These do change (we changed `build`, and `commander_modules.h` evolves constantly), so **a refresh
mechanism is always needed** — that's `regen`'s permanent floor. Shims and `regen` compose: `regen`
is the catch-all that always works; converting a script to a shim *removes it from `regen`'s scope*.

## Path forward

Adopt shims **incrementally**, highest-drift scripts first, each conversion shrinking `regen`'s
surface. `install-broker` is converted; `deploy-sbc` / `restore-arduino` / `enable-flash-boot`
(standalone, post-build, unoq board-management) are the natural next candidates. Leave the bootstrap
layer (`build`/`bum`, extra_scripts) to `regen`. No big-bang rewrite; no aggregator command — the
three atomic commands + the layer model are the streamlined surface.
