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

## 6. Framework-aware debugging, part 1: gdb helpers that know commander — *proposed (do this first)*

**Why this before the plumbing.** Commander already flashes over SWD from four
places (item 7), so *flashing* is a consolidation job. What genuinely doesn't
exist is **debugging**: nothing in the framework helps you answer "why is my
module doing nothing?" — and that question cost most of a week in Aug 2026. Two
of its causes were invisible from outside and trivial to see from inside:

- a module registered but **never pumped** (`tick()` not in the ticker list) —
  presents as dead hardware, identical to a wiring fault;
- a command silently **dropped** because `MAX_COMMANDS` was too small (this
  historically ate `ota`).

Both are one gdb command away *if* the helper knows commander's structures.

**What to build.** A gdb Python script in commander — suggested home
`scripts/gdb/commander.py` — loaded by the generated `debug` script (`gdb -x`).
Commands and the data behind them:

| command | reads | notes |
|---------|-------|-------|
| `cmdr-commands` | `CommandRegistry::_commands[]`, `_count` | print name/help/i2c_id; **loudly flag `_dropped > 0` and `_firstDropped`** |
| `cmdr-tickers` | `UartTransport::_tickers[]`, `_tickCount`, `_tickDropped` | the "module never ticks" bug, in one command |
| `cmdr-modules` | registered modules | call/inspect `IModule::name()` via the vtable |
| `cmdr-panic` | where `commander_on_panic` fired | it's a weak symbol in `core/CommandRegistry.cpp` |

Exact fields (verify against the headers before writing, they may have moved):
- `core/CommandRegistry.h`: `Command _commands[kMaxCommands]; size_t _count;
  size_t _dropped; const char *_firstDropped;` and
  `struct Command { const char *name; const char *help; uint8_t i2c_id;
  void (*handler)(const char*, Writer&, void*); void *ctx; }`.
- `transport/uart/UartTransport.h`: `IModule *_tickers[kMaxTickers]; uint8_t
  _tickCount; uint8_t _tickDropped;` with `kMaxTickers = COMMANDER_MAX_TICKERS`.

**Practicalities a fresh session will hit.**
- These are *private* members. gdb reads them fine given DWARF; no code change
  needed. Do **not** add accessors just for this.
- The registry and transport in a cmdr project are `static` objects in the
  generated `commander_modules.h` / the runner — internal linkage, but present in
  DWARF. Finding them may need `info variables` rather than a global symbol name.
- Builds are `Release` today. The Pico SDK still emits debug sections (checked
  2026-08-28: 9 of them, and commander's types are visible), so this works — but
  `-O3` will lie about line numbers and optimise away locals. See the open
  question in item 7 about build type.

**How to test it without hardware.** The host test binaries already contain a
real `CommandRegistry` (see `core/tests/test_registry.cpp` and the fake-HAL
suites under `tests/fakes/`). Build one with `-g`, run `gdb -x` against it with a
breakpoint after registration, and assert on the helper's output. That makes this
the rare debugging feature with a genuine regression test — worth doing, because
these helpers break silently whenever a struct field is renamed.

---

## 7. Framework-aware debugging, part 2: consolidate SWD behind `cmdr enable debug` — *proposed*

**The actual state.** Commander flashes over SWD in four places, none sharing a
config or a concept, and none offering gdb:

| where | what it does |
|-------|--------------|
| `tools/cmdr/src/cmdr/templates/flash-bluepill-bootloader` | `openocd -f interface/stlink.cfg -f target/stm32f1x.cfg` |
| `tools/cmdr/src/cmdr/templates/unlock-bluepill` | openocd again, with its own PATH/PlatformIO discovery |
| unoq `flash` template (in `cli.py`, look for `pkill -f openocd`) | openocd-over-adb, forwards tcp:3333 |
| `cmdr-pico-breadboard-kit/{openocd.cfg,swd-flash,swd-debug,swd-reset}` | the CMSIS-DAP reference implementation — **generalise from this one** |

**What to build.** `cmdr enable debug`, in the same shape as `cmdr enable
ota|littlefs|dfu` (copy that structure — the `_ota_*` / `_littlefs_*` helpers in
`cli.py`, plus a `cmdr.toml` record). It asks the probe type and emits
`openocd.cfg`, `debug`, `reset`, and rewires `upload` to prefer SWD with a USB
fallback.

Per CLAUDE.md's stated direction these should be **thin shims delegating to a
`scripts/swd.sh` in commander**, so the logic refreshes with `cmdr pull` rather
than going stale in every project. `install-broker` (`dev/unoq/install_broker.sh`)
is the existing precedent for that pattern. Done this way, the four integrations
above can collapse onto it later instead of becoming a fifth copy.

**Target → openocd config** (verify each; only the first two are confirmed):
- `pico` → `target/rp2040.cfg`; `pico2` → `target/rp2350.cfg` ✔ used daily
- `bluepill` → `target/stm32f1x.cfg` ✔ already in the bootloader template
- `unoq` → STM32U585, but it flashes over **adb**, not a local probe — likely
  stays special; fold in only if it's clean
- `r4` → Renesas RA4M1. **Unverified** that a usable openocd target config
  exists; check before promising the target
- `esp32` → **not applicable** (Xtensa, not ARM). Don't offer it.

**Interfaces:** `interface/cmsis-dap.cfg` covers the Waveshare RP2350-GEEK, the
Pi Debug Probe and picoprobe; `interface/stlink.cfg` covers the bluepill's
existing story. Make it a question, default cmsis-dap.

**A gotcha already paid for:** RP2350 needs
`rp2350.dap.core1 cortex_m reset_config sysresetreq` in the config, or a reset
with both cores running leaves core1 somewhere gdb can't recover. It's in the
kit's `openocd.cfg` with a comment.

**Honest menu:** only offer this where a probe applies, and remember it's opt-in
precisely because emitting a `debug` script for someone with no probe is the same
dishonesty the module menu already avoids.

**Open questions.**
- *Build type.* Debugging wants `RelWithDebInfo`. Should `cmdr enable debug`
  change it, warn, or leave it? Changing a project's optimisation level as a side
  effect of enabling a feature is a big hammer.
- *Should `upload` prefer SWD?* It's strictly better when a probe is attached —
  it verifies, and it recovers a bricked target that can't be told to enter
  BOOTSEL. But `swd-flash` currently hardcodes one project's ELF path and needs
  generalising first.
- *Does `debug` surface the probe's own view?* [cmdr-probe] knows the target's
  run/halt state by snooping DAP traffic and can report it (`probe`) — but that's
  one specific probe, and the framework shouldn't assume it.

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
