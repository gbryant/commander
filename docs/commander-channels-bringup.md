# Channel bus — bring-up & HW proof (Arduino Uno Q)

Run the Phase 2a channel bus end-to-end: commander on the STM32U585 (M33, via the Zephyr HAL)
publishing tagged streams over `lpuart1`/`ttyHS1`, and the Python broker on the QRB2210 Debian
side demuxing them. Design: `commander-channels-design.md`. Link/access: `unoq-access.md`.
HAL: `zephyr-hal-spike.md`.

**The proof:** press an IR remote button at the M33 and watch it arrive on the SBC as a
tagged `ch1` event — an unsolicited MCU→SBC stream — while the `ch0` shell still works. That's
the thing a single console can't do. (No IR remote handy? The same path works with a heartbeat
— see the end.)

The runner owns `main()` and the transport thread; your app just registers modules and wires
the publisher. Defining `COMMANDER_ENABLE_CHANNELS` swaps the UART console for the channel bus.

## 1. MCU side — the app (4 small edits to the Zephyr app)

Starting from the working spike app `~/zephyrproject/cmdr-unoq-spike/`:

**`src/main.cpp`** — register the IR module, bind it to a ch1 publisher in the bus hook:
```cpp
#include "commander.h"
#include "core/SystemModule.h"
#include "transport/channels/ChannelBusRunner.h"
#include "platform/zephyr/ZephyrIRModule.h"

extern "C" CommanderConfig commander_config() { return CommanderConfig{}; }  // defaults

static SystemModule   _sys;
static ZephyrIRModule _ir;
static ChannelTransport::ChannelPublisher _irPub;     // outlives the hook

extern "C" void commander_setup(CommandRegistry &reg) {
    reg.registerModule(_sys);                         // help/version on ch0
    reg.registerModule(_ir);                          // `ir recv`
}

// Runner calls this after setup, before the bus thread starts.
extern "C" void commander_on_channel_bus_ready(ChannelBusRunner &bus) {
    _irPub = bus.channels().publisher(1);             // ch1 = ir events
    _ir.setOutput(&_irPub);                           // each NEC press frames onto ch1
    bus.addTicker(_ir);                               // ISR decodes; tick() publishes
}
```

**`CMakeLists.txt`** — enable the bus + swap UART for the bus runner, add the IR module:
```cmake
add_compile_definitions(COMMANDER_ENABLE_CHANNELS)
target_sources(app PRIVATE
    src/main.cpp
    ${COMMANDER}/runners/zephyr/runner.cpp
    ${COMMANDER}/core/CommandRegistry.cpp
    ${COMMANDER}/transport/channels/ChannelBusRunner.cpp   # was transport/uart/UartTransport.cpp
    ${COMMANDER}/platform/zephyr/ZephyrIRModule.cpp
    ${COMMANDER}/hal/zephyr/hal.cpp
)
```

**`prj.conf`** — add: `CONFIG_GPIO=y`

**`app.overlay`** — map the IR pin (keep the existing console routing):
```dts
/ {
    chosen { zephyr,console = &lpuart1; zephyr,shell-uart = &lpuart1; };
    zephyr,user {
        ir-gpios = <&arduino_header 11 GPIO_ACTIVE_HIGH>;   // D5 — verify the index in the board dts
    };
};
```

**Wire** the TSOP/Grove IR receiver: VCC→3V3, GND→GND, OUT→**D5**. Then `west build` + flash as
in `zephyr-hal-spike.md`.

## 2. SBC side — run the broker

The broker must be the sole owner of `ttyHS1`, so stop the USB-CDC bridge first. On the board
(`adb shell` or `ssh arduino@gandalf`):
```bash
adb push transport/channels/broker/commander_broker.py /tmp/    # or scp; from the repo
sudo systemctl stop commander-bridge.service                    # frees /dev/ttyHS1
pip install pyserial                                            # once
python3 /tmp/commander_broker.py --port /dev/ttyHS1 --channels 1 --log
```
`--log` prints every inbound frame as `[chN] b'...'` — that alone is enough to see the proof.

## 3. The proof — two commands

IR receive is off until you turn it on (one command on ch0), then press buttons:
```bash
# in a second shell on the board:
echo "ir recv" > $(readlink /tmp/commander/console)
```
Now press remote buttons. The broker `--log` shows:
```
[ch0] b'listening... (ir recv to stop)'      # the command reply, framed back on ch0
[ch1] b'0x20DF10EF p3'                         # each press, tagged on ch1
[ch1] b'0x20DF40BF p3'
```
**Pass = presses appear on `ch1` as `0xHHHHHHHH p3` while ch0 still carries the shell** —
source-tagged, demuxed, concurrent MCU→SBC. (`p3` = NEC. Same wire format as the Arduino
IRModule via `modules/ir/IrEvent.h`, so host-side `irlookup` works regardless of board.)

To consume ch1 from your own process instead of eyeballing `--log`:
```bash
socat - UNIX-CONNECT:/tmp/commander/ch1.sock     # one line per press
```
And the ch0 shell interactively (optional): `screen $(readlink /tmp/commander/console)` then
type commands (no local echo — commander is message-oriented; the broker frames whole lines).

## If something's off
The decoder + transport + broker are all host-tested (`transport/channels/tests/run.sh`,
`modules/ir/tests/run.sh`), so failures localize:
- **Nothing on ch1, but `listening...` showed on ch0** → framing/transport are fine; it's the
  IR capture. Check the two HW assumptions: the `arduino_header` index = D5, and that
  `k_cycle_get_32` runs at CPU-clock (µs) resolution on the U585.
- **Garbled codes** → timing/decoder tuning, not protocol (the decoder is verified) — usually
  the cycle-counter resolution.
- **Nothing at all on ch0 either** → the link/broker: confirm `commander-bridge.service` is
  stopped and the broker says `up on /dev/ttyHS1`.

## Heartbeat variant (no IR hardware)
For a pure-software smoke, register a tiny ticker module instead of IR that publishes
`"beat N"` on ch1 each second (via `_pub.writeln`, with `_pub = bus.channels().publisher(1)`);
watch it tick on ch1 while you use the ch0 shell. Same path, no remote needed.

## Notes
- More consumers: `--channels 1,2,3` → a `/tmp/commander/chN.sock` each, fan-out to every
  connected client. Any module publishes on its own channel via the `ChannelPublisher` seam.
- The broker owns `ttyHS1`, so the Mac USB-CDC path is down while it runs;
  `start commander-bridge.service` to restore it.
