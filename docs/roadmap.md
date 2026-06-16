# commander — roadmap / framework directions

Forward-looking framework work, captured 2026-06-14 after the Arduino Uno Q / channel-bus push.
commander **works and is coherent today** — none of this is urgent. These are about *which kind
of growth stays easy*. Two paths share one foundation: if commander stays roughly "complete," the
work is making it a trustworthy, shareable artifact (tests, CI, docs, pinned releases); if it
keeps evolving, the **channel bus is the spine** to invest in. The hardening work serves both, so
it's the safe first bet.

Status key: **proposed** (not started).

---

## 1. Tests + CI for `cmdr` and a platform build matrix — *DONE (local runner) 2026-06-16*

> **Detailed plan + what landed: `docs/testing.md`** (tiered pyramid — host C++ unit tests, `cmdr`
> golden-file codegen tests, generated-project compile smoke, build matrix — plus the
> toolchain-detecting local runner). All four tiers are implemented as local runners
> (`tests/run.sh` = Tier 0/1 pre-commit gate; `tests/build-matrix.sh` = Tier 2/3). A GitHub-Actions
> CI wrapper is the only remaining piece, and is optional given the local matrix.

**Why (the biggest non-glamorous win).** `cmdr` is now the heart of the UX — ~2,300 lines of
Python — with **zero tests**. Real bugs this session (a duplicate include, a stale editable
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

## 2. Console as just another channel — the session/channel unification — *proposed*

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

## Cheap insurance (do regardless of path)

- **Protocol versioning.** A version byte in the channel handshake (and `i2c_ids.h` is already the
  I2C wire contract) so firmware and host tools that evolve independently fail *loud*, not silent.
  Increasingly important once the project is public and consumers update on their own cadence.
- **Pinned releases + a public README.** Scaffolds FetchContent `GIT_TAG main`, which is a
  reproducibility hazard for shared projects — tag releases and pin. Turn `PHILOSOPHY_NOTES.md`
  (the uncommitted seed) into real getting-started docs.
