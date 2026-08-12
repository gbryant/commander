# Design note: making the channel bus first-class

**Status:** Phase A + B1 IMPLEMENTED 2026-06-17 (Uno Q + tooling only; the six UART boards are
untouched). Remaining: B2 (console/UartTransport collapse — deferred, measured AVR gate), C
(portability), D (cmdr channel modeling), E (handshake/binary). Captured 2026-06-15, after Phase 2a
landed and was
HW-confirmed on the Arduino Uno Q (a Sony press on the M33 streamed to Debian on ch1 while the ch0
console ran concurrently — see `docs/commander-channels-design.md` and
`docs/commander-channels-bringup.md`).

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

### Phase A — Channel identity *(DONE 2026-06-17)*

A single source of truth analogous to `i2c_ids.h`: **`include/channel_ids.h`** declares
`CH_CONSOLE = 0`, `CH_IR = 1`, `CH_TOOLS = 2`, … plus a `ChannelDesc` per channel (id, name,
direction pub/sub/both, payload kind text|binary, **`command_session`**). The broker mirrors it
(`CHANNELS` in `commander_broker.py`) and derives its exposed channel set from it (no more
hand-maintained `--channels` default); the `test_channel_ids_sync.py` guard fails the suite if the
mirror drifts. `cmdr`'s unoq IR codegen now emits `publisher(CH_IR)`, not a literal `1`.

- **Killed** the "pick a free int in three files" problem (gap #3); precondition for D.
- **Invariant:** ch0 stays reserved for the console binding.
- **Dropped from the original plan:** the `channels` console command — it would only print the
  compile-time table (no runtime discovery like `i2c scan`), and the same info lives in the manifest
  / a future host-side `cmdr channel list`. Not worth an MCU command slot.

### Phase B — Unify console and channel *(the architectural keystone)*

Realize roadmap #2. Split in implementation:

**B1 — multi command-session routing *(DONE 2026-06-17)*.** `ChannelTransport::route()` now dispatches
*any* channel flagged `command_session` in `channel_ids.h` (ch0 console, ch2 tools, …) through the
`CommandRegistry` on its **own** `ChannelWriter`, so the reply (and any async stream) frames back on
the originating channel. Several host processes each get an isolated shell over the one link; ch0
behaves exactly as before (it's just a command session by descriptor). Host-tested
(`test_transport.cpp` covers a second session). Role declaration is **static** via the descriptor —
no handshake (see below).

**B2 — collapse `UartTransport` / ch0-console onto one shared dispatch core *(DEFERRED)*.** This is
the only part with reach onto the six UART boards (it touches `UartTransport`), it delivers *no new
capability* (the real shared core — `registry.dispatch` + `Writer` — already exists), and it risks
the AVR/tiny tier. Deferred to its own task gated on a **flash+RAM size diff** on `uno`/`bluepill`/a
loaded `r4`; pairs naturally with Phase C. May not be worth doing unless it visibly simplifies.

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

## Open questions — decisions (2026-06-17)

- **How a channel declares its role → RESOLVED: static, via the `channel_ids.h` descriptor.** The
  MCU is authoritative; the broker learns the table by mirroring the header (not by a runtime
  handshake). No broker-assigned roles.
- **Handshake / protocol versioning → DECLINED for now.** This is a self-contained, co-deployed
  ecosystem (firmware + broker + tools all from one repo, shipped as a matched pair), and the
  build-time **codec↔broker byte-compat test** already guarantees the two speak the same protocol
  when built together. Runtime version *negotiation* / competing-version support would be overbuild.
  The one residual crack — **separate deployment** (OTA the MCU, stale broker service) — is left for
  a future cheap **one-byte mismatch tripwire** (a smoke detector, not versioning), not a handshake.
- **Session lifecycle → static sessions for now.** A channel flagged `command_session` is always a
  session; no dynamic open/close. Dynamic per-process session allocation (and async-stream teardown
  on close) is a later enhancement.
- **Fairness across sessions:** unchanged — the command queue is serial by design; round-robin input
  is fine until a real priority need appears.
- **Still latent for B2/later:** the Pico→R4 `bridge <cmd>` remote console and telnet are both
  latent "sessions"; unify them onto this model when B2 (the dispatch-core collapse) is done.
