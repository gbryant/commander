# Channel bus — bring-up & HW proof (Arduino Uno Q)

**HW-CONFIRMED 2026-06-13.** A Sony remote press on the M33's D5 pin decoded on-chip and
streamed to Debian on `ch1` (`0x795 p4`) while the `ch0` console stayed live — the full Phase 2a
scenario on real hardware.

> **This is the bring-up record, not the route in.** It hand-builds what `cmdr` now generates:
> the app edits below are `cmdr init unoq` + `cmdr module enable ir`, and the broker steps are
> `./install-broker`. For the worked path from nothing to a working device, follow
> [`unoq-ir-speaker.md`](./unoq-ir-speaker.md) — it reaches the same proof (an IR press arriving
> on `ch1`) without editing a file. Read this one to understand what the generated code does,
> or when building a channel app by hand.

Run the Phase 2a channel bus end-to-end: commander on the STM32U585 (M33, via the Zephyr HAL)
publishing tagged streams over `lpuart1`/`ttyHS1`, and the Python broker on the QRB2210 Debian
side demuxing them. Design: `commander-channels-design.md`. Link/access: `unoq-access.md`.
HAL + **the mandatory boot-from-flash option-byte step**: `zephyr-hal-spike.md`.

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
    _ir.setOutput(&_irPub);                           // each NEC/Sony press frames onto ch1
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
        ir-gpios = <&arduino_header 11 GPIO_ACTIVE_HIGH>;   // D5 (verified: index 11 = &gpioa 11)
    };
};
```

**Wire** the TSOP/Grove IR receiver: VCC→3V3, GND→GND, OUT→**D5**.

### Build + flash (and the one-time boot fix)
`west flash` for `arduino_uno_q` isn't working upstream yet — use the openocd-over-adb gdb-load
recipe from `zephyr-hal-spike.md` (it also has the toolchain env: `gnuarmemb` at
`/Applications/ArmGNUToolchain/...`). In short:
```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi
west build -b arduino_uno_q
adb forward tcp:3333 tcp:3333 && adb shell arduino-debug &   # one instance only
arm-none-eabi-gdb build/zephyr/zephyr.elf -batch -ex "target extended-remote localhost:3333" \
  -ex "monitor reset halt" -ex load -ex "monitor reset run" -ex detach -ex quit
```
**⚠ One-time per board:** the M33 boots its ROM bootloader, not flash, until you set the
option bytes (`nSWBOOT0=0`/`nBOOT0=1`). **Do the "Make the M33 boot from flash" step in
`zephyr-hal-spike.md` first** — without it commander never runs and everything below is silent.
Confirm with `VTOR=0x08000000` over SWD.

## 2. SBC side — run the broker (keeps the Mac console)

The broker becomes the sole owner of the link, *replacing* the USB-CDC bridge — but it keeps
your Mac console alive by bridging ch0 to the same gadget the bridge used (`/dev/ttyGS0`). It
needs `ttyHS1` (and `ttyGS0` if using `--console`) free. Several things may hold `ttyHS1`: the
**`commander-bridge.service`** socat *and* the **Arduino router stack** (`arduino-router*`), which
can come back via its `.path` trigger or after a reboot. Free them all. On the board (`adb shell`
or `ssh arduino@gandalf`):
```bash
# put the script somewhere persistent (NOT /tmp — that's cleared on reboot):
scp transport/channels/broker/commander_broker.py arduino@gandalf:~/    # or adb push ... /home/arduino/
sudo systemctl stop commander-bridge.service \
     arduino-router-serial.path arduino-router-serial.service arduino-router.service
sudo fuser -k /dev/ttyHS1 2>/dev/null     # belt-and-suspenders: kill any lingering holder
python3 ~/commander_broker.py --port /dev/ttyHS1 --console /dev/ttyGS0 --channels 1 --log
```
(No `pip install` — the broker opens the serial port with `termios` directly, no pyserial, since
a stripped Debian has neither pip nor pyserial.)
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
The receiver decodes **NEC and Sony in parallel** (whichever the remote speaks). Each press
shows on ch1; watch it on Debian via the broker `--log` (confirmed with a Sony controller):
```
[ch1] b'0x000007CC p4'        # a button  (p4 = Sony SIRC; p3 = NEC)
[ch1] b'0x00000795 p4'        # another — repeats while held (Sony auto-repeats ~every 45 ms)
```
or consume it from your own process:
```bash
socat - UNIX-CONNECT:/tmp/commander/ch1.sock     # one line per press
```
**Pass = presses stream on `ch1` while the Mac `ch0` shell stays fully usable** — source-tagged,
demuxed, concurrent MCU→SBC. ✅ Same `0xHHHHHHHH pN` wire format as the Arduino IRModule (via
`modules/ir/IrEvent.h`), so host-side `irlookup` works regardless of board.

**Gotcha:** `ir recv` *toggles*. If a press shows nothing, you may have toggled it back off —
re-send `ir recv` (look for `listening...` vs `stopped.` on ch0). And the decoder is NEC/Sony
only; an RC5/RC6/other remote won't decode (extend with another `modules/ir/*Decoder.h`).

(No USB gadget / prefer a local Debian console? Drop `--console`; the broker exposes ch0 as a
PTY at `/tmp/commander/console` instead — `screen $(readlink /tmp/commander/console)`.)

## If something's off (in the order the bring-up actually failed)
The decoder + transport + broker are all host-tested (`transport/channels/tests/run.sh`,
`modules/ir/tests/run.sh`), so failures localize:
- **Nothing at all — no ch0 response either, even `help` is silent.** Most likely the M33 is
  running its ROM bootloader, not commander. Check `VTOR` over SWD (see `zephyr-hal-spike.md`):
  `0x0bf90000` = bootloader → do the option-byte boot fix. `0x08000000` = our app. *This was the
  single biggest time sink — rule it out first.* Otherwise confirm the link: nothing holds
  `ttyHS1` (router/bridge stopped), broker says `up on /dev/ttyHS1`, and only ONE openocd is
  running (a stuck root openocd can hold the CPU halted = looks identical to dead firmware).
- **ch0 works, but nothing on ch1.** Framing/transport are fine; it's IR capture/decode. Read
  the live module state over SWD: `print _ir` — `active=1 started=1 out=0x…` means it's wired
  and listening, and `_last_cyc` changing on a press proves the edge ISR fires. If edges fire
  but no codes, it's the **protocol**: the decoder is NEC/Sony only — an RC5/RC6/other remote
  won't decode (this bit us: the test remote was Sony, not NEC). `arduino_header` index 11 = D5
  = `gpioa 11` (verified), and `SYS_CLOCK_HW_CYCLES_PER_SEC=160000000` gives good µs timing.
- **Console echoes its own output in a loop** (each reply re-dispatched as `unknown: …`) →
  the broker's ch0 PTY slave must be raw or its line discipline echoes our writes back as input.
  Fixed in `commander_broker.py`; if you see it, your broker is stale — re-copy it.

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
