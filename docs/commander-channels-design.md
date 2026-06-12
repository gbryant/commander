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
2. **Then** design the channel layer deliberately (above). Prototype on the Uno Q.
Do NOT bolt mux on reflexively before B proves the stack.
