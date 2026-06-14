# Design note: multiplexing / channels for Linux-hosted commander

**Status:** design note only — NOT started. Captured 2026-06-12 while spiking the Zephyr
HAL on the Arduino Uno Q (see `docs/zephyr-hal-spike.md`, [[project_unoq_commander]]).
Prototype target: the Uno Q, where the multi-consumer need is finally real.

## The problem
Commander today is **single-session per transport**: one UART line-editor / one console
stream, the I2C bridge serves one requester, telnet is effectively single-client. Fine on
MCU-only boards (one human, one console). But a **Linux host (Debian on the Uno Q, or a
Jetson + MCU) naturally runs many processes that all want the hardware** — perception,
teleop, a logger, autonomy — and they pile onto the one bridge link to the MCU. That is
inherently a multiplexing situation, and commander doesn't model it.

## The crux (this decides WHERE mux belongs)
- A **dumb link + a Linux-side broker is fully sufficient for request/response**: the broker
  serializes — process A's command → reply → B's command. Many consumers, one wire, zero
  commander changes. Covers a lot.
- It **breaks for concurrent async streams.** If A wants a continuous sensor stream and B
  wants a continuous event stream, the MCU emits both to its single console as
  undifferentiated text, and **the broker cannot demux that after the fact** — the origin
  was never on the wire. You can't route "this line → A, that line → B" downstream of a
  source that didn't tag it.

**Therefore: the broker can multiplex *commands*; only commander can tag *output*.** The
output-tagging half is the piece that genuinely must live in commander.

## The design: a layered split
- **Linux side — a thin broker you own** (NOT Arduino's MsgPack RPC; ~100 lines of Python/Go
  owning `/dev/ttyHS1`, exposing a unix socket / TCP). Does process↔channel fan-out; the
  natural home for policy (priorities, rate-limit, access control). Host problem, host
  resources.
- **Commander side — an optional channel layer at the transport seam.** Minimal frame
  (`channel id + payload`) so one UART carries interleaved streams; incoming commands carry
  their **origin channel**; each command's output AND any async stream it spawns is routed
  to a **per-channel `Writer`**. The **command queue stays serial** (deterministic — a
  motor command must not race a logger's query *inside* the MCU). So the only genuinely new
  machinery: demux frames in, tag writers out.

## Constraints that keep it commander-shaped
- **Optional + gated** at the transport layer — the AVR Uno / simple boards allocate nothing
  for it.
- **Core registry + modules untouched** — a module still just calls `out.writeln`; the `out`
  it's handed is now a channel-routed writer. This is the seam doing its job.
- It's the "C-lite framing" idea promoted to a first-class, optional transport capability.

## Why this beats adopting Arduino's RPC (the B-vs-C reframe)
Instead of joining Arduino's ecosystem (C = their MsgPack RPC, their conventions), commander
gets its **own** minimal channel mux — which serves the Uno Q *and every future
Linux-hosted commander identically*, in commander's idiom. Strictly more general than C, and
on your terms. (B vs C context: B = raw single `ttyHS1` stream, commander's native model,
near-zero work; C = speak Arduino RPC. This note is the principled evolution beyond both.)

## Open design questions (the real depth — get it right, don't rush)
- Frame format: length-prefix vs COBS vs a tiny header; checksum?
- Per-channel `Writer` routing: how the dispatcher knows a command's origin channel and
  hands the right writer; lifetime of a channel.
- How an **async module stream** (e.g. `aicam stream`, IR recv) targets a channel rather
  than "the one console" (today they `hal_uart_puts` to a single console — that's the thing
  that must become channel-aware).
- Backpressure / flow control over the shared UART.
- **Reconcile with what commander already half-has:** the `bridge <cmd>` remote console and
  telnet both hint at a latent **"session" abstraction = (addressable writer + input
  source)**. There's a clean unifying concept here; a channel and a session may be the same
  thing. Worth designing once, not bolting on twice.

## Sequencing
1. **B first** — raw single stream over the bridge UART; validate the whole
   flash→shell→drive stack end-to-end on the Uno Q. Feel the actual need.
   ✅ DONE 2026-06-12: commander runs on the Uno Q; reboot-proof access via
   `commander-bridge.service` (see `docs/unoq-access.md`).
2. **Then** design the channel layer deliberately (above). Prototype on the Uno Q.
Do NOT bolt mux on reflexively before B proves the stack.

---

## Phase 2 — refined design (2026-06-12) — IN DEVELOPMENT

Driven by a concrete scenario: IR receiver on the MCU pushes an event to the SBC; the SBC
later sends a command to the MCU; meanwhile a sensor streams MCU→SBC *while* the SBC streams
MCU. This breaks two commander assumptions and pins the design:

**It's PEER pub/sub, not master/slave.** Both ends initiate, unprompted (the MCU pushes IR
events; the SBC pushes commands). And it's multi-stream + bidirectional + concurrent, so a
single undifferentiated console can't work — output must be channel-tagged at the source.

### The model
- **Framed channel bus** between the two brains. Frame = `[channel][payload]`; both
  directions multiplexed independently (UART is full-duplex — MCU→SBC and SBC→MCU don't
  contend, they're separate wires).
- **Channels** (ids; static for v1): `ch0 = console` (KEEP IT — the human shell / "just
  another commander app" for debugging), `ch1.. = ir / sensor / command / ...`.
- **Peer pub/sub:** a module can **publish** unsolicited to a channel (the new capability —
  IR module does `channel("ir").publish(...)` instead of `hal_uart_puts`), and **subscribe**
  to an input channel. Command dispatch is unchanged; its output just routes back tagged to
  the requesting channel.
- **SBC broker** owns the link, demuxes channels out to SBC processes (subscriber gets `ir`;
  a publisher writes `command`).
- This GENERALIZES what commander already half-does: IR events, `aicam stream`, the Pico→R4
  `bridge <cmd>` remote console, and telnet/UART consoles are all "a channel" (= addressable
  writer + optional input source). Unify once.

### Transport: UART now, SPI only if forced — and why
**Transport-agnostic by construction** (same channel/pub-sub API; swappable physical layer).
Default = **UART** (`ttyHS1`/lpuart1). Pick the wire by workload, not now:
- **Bandwidth tiers** (115200 ≈ ~10 KB/s usable per direction): events + control + PING-class
  sensors (~2 B/reading) are *trivial*; modest 10–100 Hz streams fit; raw 16 kHz×16-bit audio
  (~32 KB/s) is ~3× over → the only thing UART can't carry.
- **Reduce at the source** (edge-AI principle): don't stream raw fat data — the MCU/DSP does
  VAD/keyword/feature extraction and publishes an *event* (handful of bytes). So even "mic"
  usually collapses to the events tier. Raw streaming is the last resort.
- **Why UART over the "more capable" SPI:** "more capable" is true only on bandwidth — the
  wrong axis here. SPI is **master/slave**: a slave can't push unsolicited, so the MCU's IR
  event couldn't reach the SBC without a bolted-on GPIO data-ready line + request/grant
  protocol — i.e. hand-building what UART gives natively. Plus Linux-as-SPI-slave is awkward
  (kernel prefers master), and SPI = new wiring/drivers/framing vs UART already working. So
  UART is the *right-fit* tool for a peer messaging bus; SPI is right for master-driven bulk
  transfer. Transport-agnostic means defaulting to UART costs nothing — drop SPI under one
  fat channel later if raw audio ever truly demands it.

### Scope split
- **2a — the MCU↔SBC bus** (this scenario): framed channel bus + publish/subscribe API + thin
  SBC broker, over UART. **← starting here.**
- **2b — external reach** (Mac over USB-ethernet/TCP): additive, later. WiFi/SSH already
  covers Debian access today.

### First development slice (UART)
Frame codec (COBS-delimited `[channel][payload]`, host-testable) → a channel-mux transport on
the MCU (ch0 = console + publish on other channels) → a `publish` API → a thin SBC broker.
Prove with the scenario in miniature: IR/heartbeat publish MCU→SBC while the console still
works on ch0, plus an SBC→MCU command. Optional/gated so the AVR tier pays nothing.

### Build status (resume here)
- [x] **`transport/channels/ChannelCodec.h`** — COBS frame codec (`channel_encode` +
      streaming `ChannelReader`). Host-tested.
- [x] **`transport/channels/ChannelTransport.h`** — channel-mux: `ch0` = console (payload is
      a full command line → `CommandRegistry::dispatch` → output framed back on ch0); other
      channels via `publish()` / `subscribe()`. Byte I/O injected (`WriteFn` out + `feedByte`
      in) → link-agnostic + host-testable. Host-tested.
- [x] Host tests: `transport/channels/tests/run.sh` (all pass — pure C++, no HW).
- [x] **Module publish API + a `commander_on_channels_ready(ChannelTransport&)` hook.**
      `ChannelTransport::ChannelPublisher` (via `ct.publisher(ch)`) is a per-channel publish
      handle that IS-A `Writer`, so an async module already emitting via `Writer&` (IR recv,
      a sensor stream) frames each line onto its OWN channel instead of the single console —
      **one `writeln()` = one frame = one event** (no `\r\n`: the frame boundary IS the event
      delimiter, unlike the console `ChannelWriter`). Raw `publish()` carries binary. Default-
      constructed it's inert (`valid()==false`), so a module holds one unwired until the app
      fills it from the weak `commander_on_channels_ready(ChannelTransport&)` hook (the runner
      calls it after constructing the transport on a bus build). Host-tested.
- [x] **Runner wiring** — `transport/channels/ChannelBusRunner.{h,cpp}`, the bus-build
      counterpart to `UartTransport` (`begin`/`addTicker`/`taskBody`): `feedByte`←`hal_uart_getchar`;
      `WriteFn` = `busWrite` loops `hal_uart_putchar` (NOT `hal_uart_puts` — frames contain 0x00);
      calls the weak `commander_on_channels_ready()` hook in `begin()`. Optional/gated — only bus
      builds compile it; the AVR/console tier keeps `UartTransport`. Host-tested
      (`tests/test_runner.cpp`, with a HAL stub). Channel ids for v1: ch0 console; ch1+ =
      ir/sensor/command (firm up as real modules are wired; the proof uses ch1 = heartbeat).
- [x] **Thin Python SBC broker** — `transport/channels/broker/commander_broker.py`: owns the
      framed link (`/dev/ttyHS1`; stop `commander-bridge.service` + the `arduino-router` stack to
      take it), COBS-deframes, bridges ch0 ↔ a PTY or `--console /dev/ttyGS0` (Mac console kept)
      + fans each chN out to `<rundir>/chN.sock`, **newline-delimited** (commander's published
      events are text lines, e.g. the canonical IR event — a consumer reads line-by-line; see
      `broker/examples/ir_consumer.py`). No pyserial (termios). Codec cross-checked vs
      `ChannelCodec.h`; broker plumbing host-tested against a PTY fake-MCU. **Future binary
      channels** need self-describing framing (length-prefix or SOCK_SEQPACKET), not a delimiter.
- [x] **HW proof on the Uno Q ✅ (2026-06-13):** a Sony remote press on D5 decoded on the M33 and
      streamed to Debian on ch1 (`0x795 p4`) while the ch0 console ran concurrently — peer
      pub/sub MCU↔SBC proven. Required: a from-scratch NEC **and** Sony decoder on Zephyr, and a
      one-time STM32 option-byte write so the M33 boots flash (it ships booting the ROM
      bootloader). **Recipe: `docs/commander-channels-bringup.md`**; boot fix:
      `docs/zephyr-hal-spike.md`.
