# A debug probe with a screen — design notes

Notes for a planned project: turn a **Waveshare RP2350-GEEK** into a debug probe
that shows what it's doing on its own LCD, and eventually shows something about
the *target's* execution.

**Tier 1 is built and hardware-confirmed (2026-08-27)** — the firmware is
[cmdr-probe], a fork of debugprobe, and its `docs/geek-lcd.md` records what the
hardware taught us. Tiers 2 and 3 below are still design notes.

## The board

RP2350, 520 KB SRAM, 16 MB flash, USB-A plug, 1.14" 240×135 ST7789 IPS LCD, a TF
card slot, a 3-pin SWD connector (Raspberry Pi debug connector spec), a 3-pin
UART connector and a 4-pin I2C connector.

| Function | Pins |
|----------|------|
| LCD (ST7789) | DC 8, CS 9, CLK 10, MOSI 11, RST 12, backlight 25 |
| SWD to target | SWCLK 2, SWDIO 3 |
| UART bridge to target | TX 4, RX 5 |
| I2C | SDA 28, SCL 29 |
| TF card (SPI) | SCK 18, MOSI 19, MISO 20, CS 23 |

Two things fall out of that table:

- **The pins already match `debugprobe`'s `board_pico_config.h`** — SWCLK 2,
  SWDIO 3, UART TX 4, RX 5. Waveshare designed the board around the stock
  firmware, so `DEBUG_ON_PICO=1 PICO_BOARD=pico2` builds and runs on it.
- That same config defines `PROBE_USB_CONNECTED_LED 25`, and on this board **GP25
  is the LCD backlight**. A stock build therefore blinks the backlight as its
  connection indicator. Harmless, but it explains the behaviour, and any firmware
  we write should take that pin over properly.

`debugprobe` v2.2.0 is FreeRTOS + Pico SDK + TinyUSB — the same stack commander
targets — with tasks for USB, DAP and the UART bridge. Its USB config is one
vendor interface (CMSIS-DAP bulk) plus one CDC (the UART bridge).

## What's actually possible

Three tiers, in ascending order of ambition and descending order of certainty.

### 1. Probe status on the LCD — DONE, hardware-confirmed

A traffic light for the target: green running, yellow halted/stepping, red for a
broken link, grey for no debugger — plus DHCSR, the DP IDCODE and transfer/fault
counts underneath.

The state is snooped from the host's own DAP traffic, which cost the debug
session nothing. **The part that was not obvious:** OpenOCD parks `TAR` at the
DHCSR address once and then polls through the MEM-AP **banked data registers**
(`BD0` at AP offset `0xD10` on ADIv6), so a snooper that only understands
`TAR`/`DRW` sees the connect handshake and then goes blind. That also matters for
tier 2 below, which has to share the link's conventions. Full write-up in
cmdr-probe's `docs/geek-lcd.md`.

### 2. Standalone SWD monitor — a sampling profiler

When no host debugger is attached, the probe has a whole SWD/DAP implementation
sitting idle. Let it drive the link itself: read the target's IDCODE, poll
`DHCSR` for run/halt, and sample the PC at a few kHz. Histogram the samples and
show *where the target is spending its time* on the LCD.

With the target's symbol table on the **TF card**, that becomes function names
rather than hex addresses — a self-contained profiler that needs no host.

The constraint to respect: while a host is driving CMSIS-DAP, the probe must not
issue transactions of its own. Standalone mode is strictly for when the DAP
interface is idle.

This is the best value-for-effort of the three, and the one that can't be bought.

### 3. Instruction / instrumentation trace

The usual objection to tracing on these boards is that the Pi 3-pin debug
connector has no SWO pin, so classic SWO tracing has nowhere to land. On RP2350
that objection doesn't apply, because **the trace never leaves the chip**:

RP2350 has an ETM and a TPIU feeding an RP-specific capture block —
`CORESIGHT_TRACE` at `0x50700000` (`hardware/regs/coresight_trace.h` in the Pico
SDK). Its `TRACE_CAPTURE_FIFO` is an 8×32-bit FIFO fed from the TPIU, drained by
DMA into the target's own RAM. The register documentation is explicit about the
intended flow: hold the FIFO flushed, arm a DMA channel on the FIFO's DREQ,
release the flush to start sampling, and set it again once the buffer is full.

So a probe can configure trace over SWD and **read the trace buffer back over
SWD** — no trace pins, no extra wiring.

That splits into two very different jobs:

- **ITM** (software instrumentation — "entered task X", "ISR fired"): simple
  packet format, decodable on the probe, displayable live. Tractable.
- **ETM** (full instruction trace): capturable the same way, but reconstructing
  flow needs the target's ELF and an ETMv4 decoder. Realistic split is capture on
  the probe, decode on the host.

Tier 3 only works against RP2350 targets. Conveniently, that includes the Pico
Breadboard Kit.

**What is *not* possible:** passively sniffing the SWD wire and reconstructing
flow while a host debugger drives the link. The probe is a pipe in that mode, and
SWD carries register transactions, not program flow.

## Chosen architecture

Fork `debugprobe` and add a commander shell on a second USB CDC.

- The board's job is being a probe. Forking upstream keeps CMSIS-DAP exactly as
  it ships and makes rebasing on new releases straightforward; the DAP layer is
  also what makes tiers 2 and 3 cheap.
- Commander comes in for the parts it's good at: `IDisplay`, `Font5x7` and the
  `st7789` driver for the screen, and a `CommandRegistry` on a second CDC so the
  probe answers `lcd`, `profile`, `trace` on its own console while the first CDC
  stays the target's UART bridge.
- USB cost: `CFG_TUD_CDC` 1 → 2, a second interface pair and endpoints in
  `usb_descriptors.c`, and `CONFIG_TOTAL_LEN` updated. Well-trodden TinyUSB work.

The alternative — a commander app that links debugprobe as a component — is
cleaner conceptually but has to reconcile two TinyUSB users (`pico_stdio_usb`
versus debugprobe's own descriptors) and makes upstream rebases harder.

## Status

- [x] `st7789` module (this is the display the probe needs) — HW-confirmed
- [x] Fork debugprobe ([cmdr-probe]); GP25 taken over as a real backlight
- [x] Tier 1 status screen — HW-confirmed 2026-08-27
- [x] Remote BOOTSEL via CMSIS-DAP vendor command (stock debugprobe has no
      picoboot reset interface, so reflashing otherwise needs the BOOT button)
- [ ] Second CDC + commander shell
- [ ] Tier 2 standalone sampling profiler (+ symbols from the TF card)
- [ ] Tier 3 ITM capture over SWD
