# Channel bus — bring-up & HW proof (Arduino Uno Q)

How to run the Phase 2a channel bus end-to-end: commander on the STM32U585 (M33, via the
Zephyr HAL) publishing tagged streams over `lpuart1`/`ttyHS1`, and the Python broker on the
QRB2210 Debian side demuxing them to local consumers. Design: `commander-channels-design.md`.
Access map + the link plumbing: `unoq-access.md`. HAL spike: `zephyr-hal-spike.md`.

The proof scenario in miniature: **a heartbeat streams MCU→SBC on ch1 while the ch0 console
still works, plus an SBC→MCU command.** That exercises source-tagged, demuxed, concurrent,
bidirectional traffic — the thing a single undifferentiated console cannot do.

## 1. MCU side — swap the console for the bus + a heartbeat publisher

In the Zephyr app (`~/zephyrproject/cmdr-unoq-spike/src/main.cpp`), replace `UartTransport`
with `ChannelBusRunner`. The heartbeat publishes from a **ticker on the bus thread** (NOT a
second thread) — all framed output goes out one thread, so frames never interleave on the
shared UART.

```cpp
#include <zephyr/kernel.h>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "transport/channels/ChannelBusRunner.h"
#include "hal/hal.h"

static CommandRegistry  reg;
static SystemModule     sys;
static ChannelBusRunner bus;

// A trivial async publisher: emits "beat N" on ch1 once a second, pumped by the bus
// thread's ticker loop (the same pattern an IR/sensor module would use to publish).
struct Heartbeat : IModule {
    ChannelTransport::ChannelPublisher pub;     // filled in by the ready hook
    uint64_t last = 0; int n = 0;
    const char *name() const override { return "heartbeat"; }
    void init() override {}
    void registerCommands(CommandRegistry &) override {}
    void tick() override {
        if (!pub.valid() || hal_time_us() - last < 1000000) return;
        last = hal_time_us();
        char line[24]; snprintf(line, sizeof line, "beat %d", ++n);
        pub.writeln(line);                       // one frame on ch1, unsolicited
    }
} hb;

// The runner calls this inside bus.begin(), handing us the begun transport.
extern "C" void commander_on_channels_ready(ChannelTransport &ct) {
    hb.pub = ct.publisher(1);                     // ch1 = heartbeat/events
}

static K_THREAD_STACK_DEFINE(bus_stack, 2048);
static struct k_thread bus_thread;

int main(void) {
    reg.registerModule(sys);
    bus.begin(reg, 115200);          // inits HAL UART (RX ISR ring) + fires the ready hook
    bus.addTicker(hb);               // heartbeat pumped on the bus thread
    k_thread_create(&bus_thread, bus_stack, K_THREAD_STACK_SIZEOF(bus_stack),
                    (k_thread_entry_t)ChannelBusRunner::taskBody, &bus,
                    nullptr, nullptr, 7, 0, K_NO_WAIT);
    return 0;
}
```

The app's `CMakeLists.txt` must add `transport/channels/ChannelBusRunner.cpp` to its sources
(alongside the commander core + `hal/zephyr/hal.cpp` it already builds). No new Kconfig.

Build + flash as in `zephyr-hal-spike.md`.

## 2. SBC side — run the broker

The broker must be the **sole owner** of `ttyHS1`, so stop the USB-CDC bridge first (it also
holds the port). On the board (`adb shell` or `ssh arduino@gandalf`):

```bash
sudo systemctl stop commander-bridge.service      # frees /dev/ttyHS1
pip install pyserial                              # once
python3 commander_broker.py --port /dev/ttyHS1 --channels 1 --log
```

`commander_broker.py` is `transport/channels/broker/` in the commander repo — copy it to the
board (or `scp`/`adb push`). It prints, on start:

```
[broker] ch0 console -> /tmp/commander/console (screen $(readlink /tmp/commander/console))
[broker] ch1 -> /tmp/commander/ch1.sock
[broker] up on /dev/ttyHS1 @ 115200
```

## 3. Observe the proof

Three terminals on the board (or split with `tmux`):

- **ch1 heartbeat (MCU→SBC stream):**
  ```bash
  socat - UNIX-CONNECT:/tmp/commander/ch1.sock
  ```
  → `beat 1`, `beat 2`, … ticking once a second.

- **ch0 console (bidirectional shell):**
  ```bash
  screen $(readlink /tmp/commander/console)
  ```
  → type `help` / `version`; commander dispatches and frames the output back on ch0. (No
  per-key echo from the MCU — the terminal/`screen` provides editing; commander is
  message-oriented.) The `--log` broker also prints every inbound frame as `[chN] b'...'`.

**Pass = the ch1 heartbeat keeps ticking while you use the ch0 shell, and a ch0 command
round-trips.** Two independent streams, tagged at the source, demuxed by the broker, over one
UART — Phase 2a proven.

## Real module on a channel — IR receive (NEC) on the Uno Q

The heartbeat proves the path; a real async source is the IR receiver. The Uno Q runs
commander on the M33 via Zephyr, which has no IRremote — so IR is a from-scratch NEC receiver:
`platform/zephyr/ZephyrIRModule.{h,cpp}` (GPIO edge ISR → cycle-counter µs timing → the
portable `modules/ir/NecDecoder.h`), publishing each press on ch1 via the same `setOutput`
seam the Arduino IRModule uses. The decoder is host-tested (`modules/ir/tests/run.sh`); the
GPIO/timing path is hardware-verified here.

**Wiring:** TSOP/Grove IR receiver → VCC 3V3, GND, OUT → **D5** on the Uno Q Arduino header.

**Overlay** (`app.overlay`) — map the IR pin + enable GPIO:
```dts
/ {
    zephyr,user {
        ir-gpios = <&arduino_header 11 GPIO_ACTIVE_HIGH>;   // 11 = D5 (verify in the board dts)
    };
};
```
`prj.conf`: `CONFIG_GPIO=y`. App `CMakeLists.txt`: add `platform/zephyr/ZephyrIRModule.cpp`.
(Needs a µs-resolution `k_cycle_get_32` — Cortex-M systick at the CPU clock on the U585.)

**Runner main** — register the IR module, hand it a ch1 publisher in the ready hook, pump it
on the bus thread. (Building on the heartbeat `main` above; drop `Heartbeat` or keep both.)
```cpp
#include "platform/zephyr/ZephyrIRModule.h"

static ZephyrIRModule ir;

extern "C" void commander_on_channels_ready(ChannelTransport &ct) {
    static ChannelTransport::ChannelPublisher irPub = ct.publisher(1);  // ch1 = ir
    ir.setOutput(&irPub);                  // each NEC press frames onto ch1
}

// in main(), after bus.begin(reg, 115200):
//   reg.registerModule(ir);
//   bus.addTicker(ir);                    // ISR decodes; tick() publishes (thread context)
```
The publisher is `static` so it outlives the hook — the module holds a `Writer*` to it.

**Run + observe:** start the broker with ch1, `screen` the console, then on the shell type
`ir recv` (toggles listening). Press remote buttons:
```bash
python3 commander_broker.py --port /dev/ttyHS1 --channels 1 --log
socat - UNIX-CONNECT:/tmp/commander/ch1.sock      # -> "0x20DF10EF p3" per press
```
**Pass = button presses appear on ch1 as `0xHHHHHHHH p3` while the ch0 shell stays live** —
an unsolicited MCU→SBC event stream, tagged at the source, concurrent with the console.
(`p3` = NEC. The wire format matches the Arduino IRModule via the shared `modules/ir/IrEvent.h`,
so the same host-side parser/`irlookup` works regardless of board.)

## Notes
- More consumers: add channels with `--channels 1,2,3`; each gets `/tmp/commander/chN.sock`
  with fan-out to every connected client. The IR module publishes on its own channel exactly
  like `Heartbeat` does — both via the `ChannelPublisher` seam.
- Mac access during a broker session: the broker owns `ttyHS1`, so the USB-CDC path is down
  while it runs. Re-`start commander-bridge.service` to get the Mac console back (or, later,
  teach the broker to also re-expose ch0 over USB-CDC — out of scope for the proof).
