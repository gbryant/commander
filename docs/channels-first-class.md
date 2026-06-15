# Design note: making the channel bus first-class

**Status:** design note only — NOT started. Captured 2026-06-15, after Phase 2a landed and was
HW-confirmed on the Arduino Uno Q (a Sony press on the M33 streamed to Debian on ch1 while the ch0
console ran concurrently — see `docs/commander-channels-design.md` and
`docs/commander-channels-bringup.md`, [[project_commander_channels]]).

Phase 2a **proved the thesis** — source-tagged, demuxed, concurrent bidirectional streams over one
UART, with a thin SBC broker. But it proved it as a **bolt-on for one board**. This note is the plan
to promote the channel bus from "an optional capability the Uno Q runner happens to wire" into a
**first-class part of commander's model** — discoverable, named, portable, and understood by `cmdr`.

## What "second-class" means today (the gaps)

1. **One board only.** Only the Zephyr/Uno Q runner instantiates `ChannelBusRunner`; every other
   board hardwires `UartTransport`. A channel build is a compile-time **fork**
   (`COMMANDER_ENABLE_CHANNELS` *swaps* the runner), not a capability you *add*.
2. **Two consoles that don't share a model.** `UartTransport` (line editor, channel-less) and
   `ChannelTransport::dispatchConsole` (ch0) independently reimplement "a console that dispatches."
   The roadmap's *console = just another channel* (roadmap #2) isn't realized — instead there are
   two consoles with two code paths.
3. **Channels are anonymous integers with no registry.** `ir = 1` is a convention smeared across
   three places that don't know about each other: the codegen (`publisher(1)` in `cmdr`'s
   `_emit_module`), the broker service (`--channels 0,1`), and the tools (`ch1.sock`). There is no
   `channel_ids.h` the way there is an `i2c_ids.h`. Adding a second publishing module means
   hand-picking a free integer in all three.
4. **`cmdr` doesn't model channels.** The ch1 assignment is hardcoded in `_emit_module`. Enable a
   second channel-publishing module and it silently collides on ch1.
5. **No handshake.** The broker and the MCU never agree on codec version or which channels exist —
   a framing mismatch is a *silent* failure (frames just vanish). The design doc already flags this
   as the one silent-failure risk.
6. **Text-only, and not in CI.** No binary path for the vision/audio future (roadmap #3); the host
   tests are run by hand.

## The plan

Five phases. The spine is **A → B**; C/D/E are independent follow-ons that get much cheaper once B
lands. None of this is urgent — commander works today — but A is the cheap fix that the next
publishing module will force.

### Phase A — Channel identity *(foundation; small; do first)*

A single source of truth analogous to `i2c_ids.h`: a new **`include/channel_ids.h`** declaring
`CH_CONSOLE = 0`, `CH_IR`, … plus a tiny descriptor per channel (id, name, direction
pub/sub/both, payload kind text|binary). The broker and the host tools read the **same** manifest —
mirror it the way `i2c_ids.h` is mirrored across platforms, or generate a small JSON/py from it so
there's one authority. Add a `channels` console command that enumerates the live table
(`ch1 ir pub text`), the way `i2c scan` makes the bus legible.

- **Kills** the "pick a free int in three files" problem (gap #3) and is the precondition for D.
- **Cost:** roughly a day; mirrors a convention already trusted (`i2c_ids.h` is *the* wire-protocol
  spec, "DO NOT diverge between platforms").
- **Invariant:** ch0 stays reserved for the console binding.

### Phase B — Unify console and channel *(the architectural keystone)*

Realize roadmap #2. Define a **`Session = (addressable Writer + optional input source)`**. ch0
becomes a session that happens to dispatch; `ChannelTransport::route()` generalizes so that *any*
channel can be marked a **command session** whose inbound frames dispatch through the
`CommandRegistry` and whose output (plus any async stream it spawns) routes back to *that same
channel's* writer. Then collapse the `UartTransport` / ch0-console duplication onto one shared
dispatch core — a UART console becomes "the degenerate single-channel session."

- **Unlocks** many concurrent, isolated command sessions over one link: on a capable board several
  Debian processes (teleop, an IR mapper, a logger, autonomy) each open their *own* command session
  and get *their own* responses, without stepping on the human console. The board stops being "one
  serial shell." This is the multi-consumer promise realized at the **command** layer, not just the
  data layer — and it makes the channel-native IR tools clean (open a session, enable IR there, done;
  no reaching through ch0).
- **Invariant preserved:** the command *queue* stays serial and deterministic inside the MCU (a
  motor command must not race a logger's query) — sessions multiplex *input*, they don't parallelize
  execution. ch0 remains the canonical human-console binding for compatibility.
- **Biggest piece;** it changes commander's *model*, not just its plumbing. C/D fall out of it much
  more cheaply once done. Detailed in roadmap #2 — this is its implementation home.

### Phase C — Portability *(channels on every capable board, not a fork)*

Promote "2b external reach" from the design doc: a channel transport over **TCP/USB-CDC** so a
broker can demux Pico / ESP32 / R4 too, not just the Uno Q's `ttyHS1`. And — riding on B — make
channels a transport you *add* alongside the UART console, not a runner you *swap*. After B, "add
channels" is uniform across boards (it's just "bind a session to a link").

- The AVR/tiny tier still pays nothing — channels stay optional and gated; this is about the
  *capable* boards no longer being a special build.

### Phase D — `cmdr` integration *(first-class in the tool)*

Module specs declare `publishes` / `subscribes` channels **by name**; `cmdr module enable`
**allocates** ids from `channel_ids.h`, regenerates the manifest, and wires the publisher — no
hardcoded ch1, no collisions (gap #4). `cmdr channel list` shows the per-project map. The broker
service + channel tools become a **generated, version-stamped** artifact: `cmdr` emits the broker
service with the right `--channels` from the manifest, and `deploy-sbc` ships a **matched
broker+codec pair** so they can't drift. Ties into the HAL capability model (roadmap #4) so a board
that can't carry channels doesn't offer the mode.

### Phase E — Hardening / insurance

- **Handshake on connect** — broker ↔ MCU agree on codec version + channel manifest; a mismatch is
  a **loud** error, not silent vanished frames (gap #5; the "cheap insurance / protocol versioning"
  line in the roadmap).
- **Binary framing** — a length-prefix / `SOCK_SEQPACKET` path additive alongside the COBS-text
  form, for vision/audio (roadmap #3). Per-channel kind (text channels keep the newline form).
- **CI** — the existing host tests (`transport/channels/tests`) plus a permanent **codec↔broker
  byte-compat** check run in the build matrix (see `docs/testing.md`). The MCU codec and the Python
  broker drifting apart is a silent failure, so it belongs in the permanent suite, not a one-off
  manual cross-check.

## Recommendation

Start with **A** — the moment a second publishing module (a sensor stream next to IR) is enabled,
the anonymous-int collision bites, and A is the cheap fix that also unblocks D. Then commit to **B**
as the real first-class milestone; it changes the model rather than the plumbing, and C/D get much
cheaper once it's done.

## Open questions (carried from the design doc — get these right, don't rush)

- How a channel declares its role: static config (`channel_ids.h` descriptor) vs a handshake frame
  vs the broker assigning roles at connect.
- Session lifecycle: open/close, per-session writer state, what happens to an async stream when its
  session closes.
- Fairness across command sessions sharing the one serial queue (the queue is serial by design — is
  round-robin enough, or does a session need priority?).
- Reconcile with what commander already half-has: the Pico→R4 `bridge <cmd>` remote console and
  telnet are both latent "sessions." Design the session abstraction once so they unify, not twice.
