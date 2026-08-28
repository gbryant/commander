# commander — roadmap / framework directions

Forward-looking framework work, captured 2026-06-14 after the Arduino Uno Q / channel-bus push.
commander **works and is coherent today** — none of this is urgent. These are about *which kind
of growth stays easy*. Two paths share one foundation: if commander stays roughly "complete," the
work is making it a trustworthy, shareable artifact (tests, CI, docs, pinned releases); if it
keeps evolving, the **channel bus is the spine** to invest in. The hardening work serves both, so
it's the safe first bet.

Status key: **proposed** (not started) · **partially shipped** (some phases landed, rest deferred) · **DONE** (complete; kept for the reasoning). Items below carry their state in the heading — where a heading says DONE, the body is the original argument for doing it, preserved as written.

---

## 1. Tests + CI for `cmdr` and a platform build matrix — *DONE (local runner) 2026-06-16*

> **Detailed plan + what landed: `docs/testing.md`** (tiered pyramid — host C++ unit tests, `cmdr`
> golden-file codegen tests, generated-project compile smoke, build matrix — plus the
> toolchain-detecting local runner). All four tiers are implemented as local runners
> (`tests/run.sh` = Tier 0/1 pre-commit gate; `tests/build-matrix.sh` = Tier 2/3). **GitHub-Actions
> CI was deliberately declined (2026-06-16): solo-dev effort, local testing suffices for the
> foreseeable future** — revisit only with outside contributors. So this item is considered done.

**Why (the biggest non-glamorous win, as it stood in June 2026).** `cmdr` is the heart of the UX —
~3,400 lines of Python today — and at the time had **zero tests**. Real bugs that session (a duplicate include, a stale editable
install, honest-menu gating) were caught only by hand-scaffolding and eyeballing. As boards and
modules accrue, and especially as the project is shared publicly (others will depend on it), the
absence of tests becomes the bottleneck between *confident* evolution and fear-driven stagnation.

**What.**
- A **cmdr test suite**: scaffold each board (`cmdr init <board>`), enable each module, and assert
  the generated `commander_modules.h`, the dev/board scripts, and the honest-menu output. Pure
  Python, fast, no hardware. This pins the codegen + the per-platform wiring (e.g. the unoq
  channel-bus hook vs the UART hook).
- A **build matrix CI**: at minimum *configure/compile* each platform target. The host C++ tests
  (`transport/channels/tests`, `modules/ir/tests`) already cover the portable logic; the gap is a
  per-platform build breaking silently when core/modules change.

**Payoff.** Every future change becomes safe; the project becomes shareable without "works on my
machine." Precondition for everything below.

---

## 2. Console as just another channel — the session/channel unification — *partially shipped 2026-06-17*

> Phases A (channel identity, `channel_ids.h`) + B1 (per-channel command sessions)
> shipped — see `docs/channels-first-class.md` and PLAN.md ("Channel bus"). B2 (ch0
> console collapse), C (more boards), D (cmdr modeling), E (handshake) remain proposed.

> **Implementation home: `docs/channels-first-class.md`** (Phase B). That note frames this unification
> as the keystone of a broader plan to make the channel bus first-class — channel identity
> (`channel_ids.h`), portability across boards, `cmdr` integration, and a connect handshake.

This is the cleanest architectural move and the one that makes the channel bus *general* instead
of "console plus pub/sub." It is the highest-value evolving-path work.

**The problem.** Today **ch0 is special**: it is the console, it owns the line editor, and it is
the *only* channel that dispatches commands. Every other channel is data-only pub/sub. That bakes
in the single-serial-console assumption — even on a capable board running the channel bus, there
is effectively one command session. (Concretely: the Uno Q IR tools had to reach through ch0 to
send `ir recv`, sharing the one console with the human; two consumers issuing commands would
collide. The "IR tools shouldn't need ch0" instinct is this problem surfacing.)

**The model.** Generalize a channel into a **session = (addressable writer + optional input
source)**. A channel can be:
- a **command session** — its inbound frames are dispatched through the `CommandRegistry`, and the
  command's output (plus any async stream it spawns) routes back to *that same channel's* writer;
- a **data channel** — pub/sub only (what chN is today);
- **ch0** becomes just a command session that happens to have a human terminal (PTY / USB-CDC)
  bound to it — no longer special in the protocol, only a default binding.

**On the MCU.** `ChannelTransport::route()` currently special-cases `ch0 → dispatchConsole` and
`chN → subscribers`. The unified version: a channel may be marked a command session; `route()`
dispatches its input through the registry using a **per-channel `ChannelWriter`** so the response
frames back on the originating channel. Line-editing/echo/prompt already live in the host broker
for the bus, so the MCU just sees framed command lines per channel — minimal new machinery (a
per-session writer + a small "is this channel a command session" flag).

**What it unlocks.** Multiple **concurrent, isolated command sessions over one link**. On a capable
board (the Uno Q, or a Jetson + MCU), several Debian processes — teleop, an IR mapper, a logger, an
autonomy loop — each open their *own* command session, issue commands, and receive *their own*
responses, without stepping on each other or on the human console. The board stops being limited to
"one serial shell." This is the multi-consumer promise the channel bus exists for, finally realized
at the **command layer**, not just the data layer — and it makes the channel-native IR tools (and
any future tool) clean: open a session, enable IR there, done, no ch0.

**Invariant preserved.** The command *queue* stays serial and deterministic inside the MCU — a
motor command must not race a logger's query — sessions multiplex *input*, they don't parallelize
execution. ch0 remains the canonical human-console binding for compatibility.

**Open questions.** How a channel declares its role (static config vs a handshake frame vs the
broker assigning roles); session lifecycle (open/close, per-session writer state); fairness across
sessions. Worth designing once, deliberately — it touches the protocol.

---

## 3. Binary channel framing — *proposed (explore)*

**Why.** Channels today are text / newline-delimited (the broker fan-out delimiter; the canonical
IR event line). The Uno Q + the Grove AI-cam work point at non-text payloads — audio, image tiles,
structured/packed sensor data. A newline delimiter can't carry arbitrary bytes.

**What.** A self-describing framing for binary channels: **length-prefix** per message, or a
`SOCK_SEQPACKET` socket on the broker side (preserves message boundaries natively). Likely a
**per-channel type** (text/event channels keep the newline form; binary channels opt into framed).
The COBS wire format between MCU and broker already preserves frame boundaries; this is about the
broker→consumer hop and the payload contract. Keep "reduce at the source" in mind (the edge-AI
principle) — most "binary" needs collapse to small events; raw streaming is the last resort.

---

## 4. HAL capability model — *proposed*

**Why.** The honest menu is currently a **hardcoded allowlist** (`UNOQ_MODULES = {system, ir}`)
because the Zephyr HAL is incomplete (GPIO/I2C stubbed) and a module has no way to declare "I need
GPIO." That's a maintenance smell: every new board with a partial HAL needs another allowlist edit.

**What.** Have each platform's HAL **advertise the capabilities it backs** (i2c, gpio, pulse-in,
uart, …), and each module **declare the capabilities it needs**. `cmdr`'s available-module menu is
then *derived automatically* — no allowlists, no enable-but-doesn't-work traps. Porting to a new
board becomes "implement these capabilities," and the module menu lights up as the HAL grows.

**Payoff.** Makes the "generic Zephyr tier / more boards for free" future real, and turns the
honest-menu principle from a hand-maintained list into an invariant.

---

## 5. Async streams need a writer that outlives one command — *proposed*

**The problem.** A `Writer` is a stack local owned by a single `dispatch()` — `UartWriter out;` in
`transport/uart`, `TelnetWriter out(fd);` in `transport/telnet`. It dies when the command returns.
But a module's *stream* outlives the command that started it: `ir recv`, `aicam stream` and the
`watch` modes on `touch` / `joy` / `btn` all emit from `tick()`, in the UART task, long afterwards.

There is currently no writer such a module may legally hold, so **every one of them writes to the
board console** via `hal_uart_puts` (see `modules/ConsoleOut.h`). Start `touch watch` over telnet
and you get the acknowledgement on telnet and the stream on the serial port — which is not what
anyone means by it.

This is not a hypothetical: a module storing the `Writer*` it was handed is a use-after-free, and
that shipped briefly on 2026-08-27 before being caught and routed to the console instead. The
console was the only sink guaranteed to still exist.

**The fix.** A small **sink registry in `core/`**, with lifetime owned by the transports: register a
Writer when a session opens, unregister when it closes; tick-driven modules emit to the registered
sinks, falling back to the console when there are none. One change fixes every streaming module,
and both serial and telnet see the output.

**Risks worth designing for, not discovering.**
- *Cross-task writes.* The UART task would call `lwip_send` on telnet's socket. The Pico runner uses
  the `lwip_sys_freertos` flavour, whose socket API is thread-safe — verify rather than assume, and
  check the ESP32 runner separately.
- *A stalled client must not stall the board.* If a telnet socket blocks, the tick loop that also
  polls touch and buttons stalls behind it. Needs a non-blocking send with an explicit drop policy,
  so a wedged client degrades its own stream and nothing else.

**Relation to item 2.** The session/channel unification wants exactly the same thing — "the
command's output *plus any async stream it spawns* routes back to that same channel's writer" — but
for the channel bus. These are one abstraction seen from two directions: an addressable writer that
outlives a dispatch. Worth checking whether one mechanism serves both before building either.

**Affected today:** `ir recv`, `aicam stream`, `touch watch`, `joy watch`, `btn watch`.

---

## 6. Framework-aware debugging, part 1: gdb helpers that know commander — *written 2026-08-28, NOT YET RUN*

`scripts/gdb/commander.py` implements the four commands below, and
`scripts/swd.sh` loads it automatically when `./debug` starts.

| command | what it answers |
|---------|-----------------|
| `cmdr-commands` | the command table; loudly flags `_dropped > 0` and names `_firstDropped` |
| `cmdr-tickers` | which modules are actually pumped — the "module never ticks" bug in one command |
| `cmdr-modules` | registered modules by concrete class, with whether each ticks and what it registered |
| `cmdr-panic` | whether the target is spinning in `commander_on_panic`, and who called it |

**The blocker, found on first use: the gdb in Arm's own toolchain has no Python
support.** `arm-none-eabi-gdb --batch -ex "python pass"` reports *"Python
scripting is not supported in this copy of GDB"* on the Arm GNU Toolchain
14.2.rel1 macOS build (`/Applications/ArmGNUToolchain/`, the `gcc-arm-embedded`
cask). gdb then reads the `.py` as a *command* file and fails with `Undefined
command: ""`. So **none of these four commands has ever executed.** They are
written against verified struct fields and are syntactically valid Python,
nothing more than that.

`swd.sh` now probes for Python support and skips the helpers with a note naming
the fix, so a non-Python gdb degrades cleanly instead of erroring at startup.

**To finish this item**, someone needs a Python-enabled ARM gdb — Homebrew's
`arm-none-eabi-gdb` formula builds from source and normally has it; the xPack
distribution bundles Python too — and then to actually run the four commands
against a live target and fix what falls over. Expect the object-finding to need
the most work: the registry and transport are `static` objects with internal
linkage, so `_resolve()` parses `info variables` output by type rather than
looking up a name, and that parsing is the least verified part.

**Design notes worth keeping:**
- Everything is read-only and calls **nothing** on the target. Module identity
  comes from each vtable pointer's `dynamic_type`, which gdb resolves without an
  inferior call — so the helpers work on a target that is halted, faulted, or in
  no state to run code. This is a deliberate departure from the original sketch,
  which proposed calling `IModule::name()` through the vtable.
- These are private members and stay that way. No accessors were added for
  debugging; gdb reads them from DWARF.
- The silent-breakage risk (a field rename kills the helpers with no compile
  error) is covered by `tools/cmdr/tests/test_gdb_helpers.py`, which asserts
  every field name the script reads still exists in the header it reads it from.
  That runs everywhere; a gdb-driven test cannot be the gate while toolchains
  ship without Python.
- Builds are optimised by default, so line numbers jump and locals vanish.
  `cmdr enable debug` says so and leaves the build type alone (see item 7).

---

## 7. Framework-aware debugging, part 2: consolidate SWD behind `cmdr enable debug` — *DONE 2026-08-28 (v1.3)*

`cmdr enable debug [--probe cmsis-dap|stlink|jlink]` emits `openocd.cfg` plus
`flash` / `debug` / `reset`, recorded as a `[debug]` section in `cmdr.toml` and
gitignored like the other generated scripts. Offered for **pico, pico2 and
bluepill** only. See `docs/cmdr.md`.

**How it came out, against what was proposed:**

- **Thin shims, as directed.** The logic is `scripts/swd.sh` in the framework;
  the project scripts locate it and exec it, so fixes travel by `cmdr pull`
  instead of going stale in every consumer. `install-broker` was the precedent
  and this is the second one. Resolution order is `$COMMANDER_PATH`, then
  `FETCHCONTENT_SOURCE_DIR_COMMANDER` from the CMake cache (`cmdr link`), then
  the FetchContent copy, then PlatformIO's `lib_deps` — and the failure message
  distinguishes "not fetched yet" from "fetched but predates v1.3".
- **`r4` is confirmed out.** openocd 0.12 ships no `renesas_ra` target config
  (checked 2026-08-28), so it would have emitted a script that always fails.
  `esp32`, `uno` and `unoq` are out for the reasons already given.
- **The RP2350 core1 `sysresetreq` fix is generated** for pico2 and not for pico,
  with the comment explaining why, so nobody has to pay for it twice.
- **`upload` was NOT rewired** (decided 2026-08-28). It was proposed, but a
  `./bum` that silently changes what it does depending on what is plugged in is
  worse than two scripts that each do one thing. `./upload` is BOOTSEL, `./flash`
  is SWD.
- **Build type: warns, doesn't change.** Enabling a feature must not silently
  alter the image you flash, so `cmdr` prints the `RelWithDebInfo` line and
  leaves the choice alone. This is the open question from the original item,
  answered.
- **`[debug]` survives a debug-unaware `write_manifest`**, the way `[autostart]`
  and the `libraries_only` top key do. Without it, `cmdr module enable` would
  drop the section and orphan the scripts. Covered by a test that exists
  specifically to fail if someone reworks the manifest writer.
- **Version skew is feature-scoped.** `DEBUG_MIN_TAG = "v1.3"` rather than a
  `MIN_FRAMEWORK_TAG` bump — nothing else cmdr generates needs v1.3, and forcing
  every v1.2 project to move just to keep using `cmdr module enable` would be
  the guard doing more harm than the skew.

**Fixed in v1.4:** enabling it on a `libraries_only` project wrote and
gitignored the four files, and then `cmdr regen` never put them back — the SWD
emission sat *after* the libraries-only early return in `_emit_scripts`, so a
fresh clone of such a project silently lost its tooling. The files are not part
of the build (they need only the ELF's directory), so they now emit ahead of
that gate, and `[debug].build_dir` records the directory explicitly —
required via `--build-dir` for libraries-only projects, since cmdr cannot know
where their ELF lands (cmdr-probe links into `build-geek`, not `build-pico2`).

**Still open, deliberately:** the four other openocd call sites
(`flash-bluepill-bootloader`, `unlock-bluepill`, the unoq `flash` template) have
NOT been collapsed onto `swd.sh` yet. They work, and folding them in is a
refactor with its own hardware-verification cost. `swd.sh` is now the thing to
fold them onto rather than a fifth copy to add to. Item 6's gdb helpers are also
still to do — `swd.sh`'s `debug` already loads `scripts/gdb/commander.py` if it
finds one, so that item only has to write the file.

---

## Cheap insurance (do regardless of path)

- **Protocol versioning.** A version byte in the channel handshake (and `i2c_ids.h` is already the
  I2C wire contract) so firmware and host tools that evolve independently fail *loud*, not silent.
  Increasingly important once the project is public and consumers update on their own cadence.
- **Pinned releases + a public README** — *DONE 2026-08*. `v1.0` tagged, scaffolds emit
  `GIT_TAG <release>` via `FRAMEWORK_TAG`, and the getting-started/module/cmdr docs are written.
  Releases are two-part `vMAJOR.MINOR`; the major moves only when a release breaks consumers.
- **Audit the HAL for peripherals left in an assumed state.** Two bugs on 2026-08-27 were the same
  species: `hal_i2c_write_read` held the bus with no STOP when no read followed (the GT911's
  touch-acknowledge write never completed), and `hal_pwm_stop` disabled the PWM slice without
  settling the pin, freezing it high about half the time (a buzzer stuck on). In both cases the HAL
  left the peripheral wherever the last operation happened to leave it, rather than putting it in a
  defined state. **The host tests cannot catch this class** — `tests/fakes` has its own HAL, so it
  sees what a driver *asks for*, never what the silicon is left holding. Worth reading the SPI and
  GPIO paths with the same question: after this call returns, is the hardware in a state I set, or
  one I hope it landed in?
- **Make the CMake-generated dev scripts relocatable.** `cmake/GenerateScripts.cmake` substitutes
  `@CMDR_BUILD_DIR@` / `@CMDR_SOURCE_DIR@` as *absolute* paths, so on pico/pico2 the emitted
  `build`/`upload`/`monitor`/`bum-ota` hard-code the project's location. Renaming or moving a
  project silently breaks them until the next `cmake` configure — they point at a directory that
  no longer exists. Four of the five could use `$DIR` (as the cmdr-emitted PlatformIO/IDF scripts
  already do); `build` is the exception, since it bakes `$BLUEPAD32_PATH`, which lives outside the
  project by nature. Worth doing for robustness. It does *not* make the scripts committable —
  `configure_file` rewrites them on every configure, so a tracked copy would be permanent
  working-tree noise; they stay gitignored either way.
