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

## 2. SBC side — run the broker (keeps the Mac console)

The broker becomes the sole owner of the link, *replacing* the USB-CDC bridge — but it keeps
your Mac console alive by bridging ch0 to the same gadget the bridge used (`/dev/ttyGS0`). So
stop the bridge (that frees both `ttyHS1` and `ttyGS0`) and hand `ttyGS0` to the broker with
`--console`. On the board (`adb shell` or `ssh arduino@gandalf`):
```bash
# put the script somewhere persistent (NOT /tmp — that's cleared on reboot):
scp transport/channels/broker/commander_broker.py arduino@gandalf:~/    # or adb push ... /home/arduino/
sudo systemctl stop commander-bridge.service                    # frees ttyHS1 + ttyGS0
pip install pyserial                                            # once
python3 ~/commander_broker.py --port /dev/ttyHS1 --console /dev/ttyGS0 --channels 1 --log
```
(`--rundir` defaults to `/tmp/commander` for the console PTY + `chN.sock` — that *is* ephemeral
runtime state, so `/tmp` is fine for it; only the script wants a durable home. Once the proof
sticks, the natural next step is a `commander-broker.service` that replaces `commander-bridge`
— same idea as that unit, see `docs/unoq-access.md`.)
With `--console /dev/ttyGS0`, your **Mac serial path is unchanged**: open `/dev/cu.usbmodem*`
as always and you get the commander shell (the broker supplies echo + line editing, since the
bus MCU no longer echoes per-key). `--log` prints every inbound frame as `[chN] b'...'`.

So you keep your two familiar terminals: **Mac `screen /dev/cu.usbmodem*`** = the commander
console (ch0), and **`ssh`/`adb` to Debian** = the Linux shell — now with the channel sockets
available there too.

## 3. The proof

In the Mac console (`/dev/cu.usbmodem*`), turn IR receive on, then press remote buttons:
```
ir recv
```
Each press shows on ch1. Watch it on Debian via the broker `--log`:
```
[ch1] b'0x20DF10EF p3'        # each press, tagged on ch1
[ch1] b'0x20DF40BF p3'
```
or consume it from your own process:
```bash
socat - UNIX-CONNECT:/tmp/commander/ch1.sock     # one line per press
```
**Pass = presses stream on `ch1` while the Mac `ch0` shell stays fully usable** — source-tagged,
demuxed, concurrent MCU→SBC. (`p3` = NEC. Same wire format as the Arduino IRModule via
`modules/ir/IrEvent.h`, so host-side `irlookup` works regardless of board.)

(No USB gadget / prefer a local Debian console? Drop `--console`; the broker exposes ch0 as a
PTY at `/tmp/commander/console` instead — `screen $(readlink /tmp/commander/console)`.)

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
- With `--console /dev/ttyGS0` the broker drives the USB-CDC gadget itself, so the Mac console
  coexists with the channels — no need to flip back to `commander-bridge.service`. (Restart that
  service only if you stop the broker and want the plain passthrough bridge back.)
