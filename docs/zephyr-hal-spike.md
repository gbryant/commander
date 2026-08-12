# Zephyr HAL backend — spike plan (steps 1 & 2)

> **This is a historical record, kept for the *why*.** The spike succeeded: the Uno Q is a
> first-class target, and every procedure below is now done for you by a generated script —
> `./build`, `./flash`, `./enable-flash-boot`, `./install-broker`. Read this to understand
> what those scripts do and which findings they encode (the `lpuart1` discovery, the
> boot-from-flash option bytes, the openocd traps). To *use* the board, start at
> [`unoq-ir-speaker.md`](./unoq-ir-speaker.md); to bring up a stock one,
> [`getting-started-unoq.md`](./getting-started-unoq.md).

**Goal:** prove whether commander can ride on Zephyr as *one more HAL backend* without
disturbing core/modules/cmdr — so capable boards Zephyr already supports (first target:
**Arduino Uno Q**, STM32U585 / Cortex-M33) come "for free" via devicetree, while the
tiny/AVR tier stays bare-metal. The bar: does the Zephyr build/devicetree/Kconfig workflow
*reduce* mental load for hardware fun, or add friction? Only a keyboard spike answers that.

**Design invariant:** Zephyr serves the HAL; it does NOT become the framework. commander's
`core/`, `modules/`, and the `cmdr` tool stay untouched. New code is only a `hal/zephyr/`
backend + a `runners/zephyr/` runner (k_thread_create instead of xTaskCreate). This dents
the "FreeRTOS everywhere" principle (Zephyr has its own kernel) — accepted, because task
creation already lives in the runner, so modules don't notice.

## STATUS — resume here
- [x] **STEP 1 COMPLETE (2026-06-12): build + flash + console all proven on the Uno Q.**
- [x] Decision: **path B** (commander owns the bridge UART, raw). Confirmed working.
- [x] **STEP 2 COMPLETE (2026-06-12): commander runs on Zephyr, `help`/`version` over
      `/dev/ttyHS1`.** Core/transport/SystemModule UNMODIFIED; only new code = `hal/zephyr/
      hal.cpp`. The HAL seam thesis is proven on hardware.
- [x] **BOOT-FROM-FLASH FIX (2026-06-13): the M33 must be told to boot flash via option bytes**
      — by default it powers into the STM32 ROM bootloader, so commander never runs. One-time
      openocd option-byte write makes it boot our firmware on every power-up. **See the new
      "Make the M33 boot from flash" section below — this is REQUIRED for any commander build.**
- [x] **Channel mux HW-CONFIRMED on the Uno Q (2026-06-13):**
      `ChannelBusRunner` + SBC broker + a from-scratch IR receiver (NEC **and** Sony). A Sony
      remote press on D5 decoded on the M33 and streamed to Debian on ch1 (`0x795 p4`) while the
      ch0 console stayed live. Full recipe: `docs/commander-channels-bringup.md`.
- [x] **DONE: promoted into commander** — `hal/zephyr/hal.cpp` + `runners/zephyr/runner.cpp`
      + `platform/zephyr/ZephyrIRModule.cpp` are in-repo, and **`cmdr init unoq` is a
      first-class target** (Zephyr/west build, gdb-load flash over on-board OpenOCD, the
      option-byte step folded into the generated `enable-flash-boot` script). The Uno Q is
      no longer a spike — see `docs/getting-started-unoq.md` and `docs/unoq-ir-speaker.md`.

### Step-2 artifacts + the key lesson
- Scratch app: `~/zephyrproject/cmdr-unoq-spike/` (CMakeLists pulls commander core + the new
  `hal/zephyr/hal.cpp`; `src/main.cpp` = SystemModule + UartTransport on a `k_thread`;
  `app.overlay` routes console to lpuart1; `prj.conf`; `include/version.h` stub).
- **Critical HAL lesson: UART RX MUST be interrupt-driven.** First cut used `uart_poll_in()`
  + `k_msleep(1)` — at 115200 the 1-byte RX register overruns between polls, so only the
  FIRST byte of each input burst survived (`> hv` instead of help/version). Fix in
  `hal/zephyr/hal.cpp`: ISR drains the FIFO into a `RING_BUF` (`CONFIG_UART_INTERRUPT_DRIVEN
  =y`); `getchar` reads the ring. TX stays `uart_poll_out`. (`uart_irq_update()` returns
  `void` in Zephyr 4.4 — call it, don't test it.)
- Minor: `version` printed "commander build 0 (unknown)" — a different `version.h` on the
  include path won over the app stub (cosmetic; sort out when wiring the runner/VersionStamp).

### KEY FINDING — the bridge UART is `lpuart1` (NOT `usart1`)
- `usart1` (PB6/PB7) = the **Arduino header Serial1 (D0/D1)** — `arduino_r3_connector.dtsi`:
  `arduino_serial: &usart1`. The board's default `zephyr,console=&usart1` goes to the HEADER
  PINS, not Debian — that's why `ttyHS1` was silent.
- **`lpuart1` (PG7 TX / PG8 RX, RTS/CTS PG6/PG5) is the QRB bridge** → `/dev/ttyHS1`. Proven:
  build hello_world with overlay `chosen { zephyr,console = &lpuart1; zephyr,shell-uart =
  &lpuart1; };`, flash, and `cat /dev/ttyHS1` showed the Zephyr boot banner + Hello World.
- So **commander's Zephyr console must target `lpuart1`.**

### CONSOLE-READ recipe (B path, confirmed working)
1. Free the bridge (needs sudo; adb shell is unprivileged user `arduino`, but it IS in the
   `dialout` group so it can read ttyHS1 once free):
   `adb shell "echo <pw> | sudo -S systemctl stop arduino-router.service arduino-router-serial.service"`
   (use `disable --now` to persist across reboots; `start` to restore stock Arduino behavior).
2. Build with the lpuart1-console overlay (see above), flash via the gdb recipe below.
3. `adb shell "stty -F /dev/ttyHS1 115200 raw -echo; timeout 8 cat /dev/ttyHS1"` while you
   `monitor reset run`.

### CONFIRMED flash workflow (copy-paste)
```
# one-time per shell: toolchain (Intel-Mac SDK gap workaround)
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi
WEST=~/zephyrproject/.venv/bin/west
GDB=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi/bin/arm-none-eabi-gdb

$WEST build -p always -b arduino_uno_q <sample> -d ~/zephyrproject/build/x   # build
adb forward tcp:3333 tcp:3333                                                # host->board openocd
adb shell arduino-debug   # leave running (bg): openocd, gpiod-SWD, gdb server on :3333
$GDB ~/zephyrproject/build/x/zephyr/zephyr.elf -batch \
  -ex "target extended-remote localhost:3333" \
  -ex "monitor reset halt" -ex load -ex "monitor reset run" -ex detach -ex quit
```
Confirmed live: openocd sees `SWD DPIDR 0x0be12477`, "Cortex-M33 r0p4", gdb load 18500 B OK.

## Make the M33 boot from flash (option bytes) — REQUIRED, hard-won 2026-06-13

> **You don't have to do this by hand.** Every scaffolded project ships `./enable-flash-boot`,
> which performs exactly the write below, is idempotent (it re-reads `FLASH_OPTR` first and
> does nothing if it's already `0x1beff8aa`), and confirms VTOR afterwards. It's **once per
> board**, not per project, and `./restore-arduino` reverts it. This section is why it exists
> and what to do when it misbehaves.

**Symptom:** you flash commander, `monitor reset run`, but the MCU is silent — no console, no
response. Even a known-good plain-console build is silent. It looks like a UART/link problem;
it is not.

**Root cause:** by default the Uno Q's STM32U585 powers into its **ROM/DFU bootloader**, not
your flashed app. The Uno Q's native model is **MPU-controlled** — the Arduino Linux stack
(`arduino-app-cli`) loads sketches (as LLExt, into RAM or flash) and manages MCU execution; the
chip isn't expected to boot its own flash standalone. So a bare commander firmware never runs.

**Diagnosis (how to confirm):** read the vector table offset register over SWD —
```
$GDB -batch -ex "target extended-remote localhost:3333" -ex "monitor reset run" \
  -ex "shell sleep 1" -ex "monitor halt" \
  -ex 'printf "VTOR=0x%08x\n", *(unsigned int*)0xE000ED08'
```
`VTOR=0x0bf90000` → running the **ROM bootloader**. `VTOR=0x08000000` → running **our app**.
(With the bootloader running, halts land at PC `0x0bf9xxxx`, often reading as `0x0`/secure —
that's just the ROM, not a TrustZone fault. `TZEN=0` on this part: TrustZone is off.)

**What does NOT fix it:** masking/un-masking the `arduino-router` stack (tested — restoring the
router + reboot left VTOR at the bootloader). The router isn't what controls MCU boot.

**The fix — force boot-from-flash in the STM32 option bytes (one-time, persistent):**
`nSWBOOT0=0` makes the chip ignore the physical BOOT0 pin and take BOOT0 from the `nBOOT0`
option bit; `nBOOT0=1` then selects main flash. (Polarity is ST-"intentionally undocumented";
determined empirically here: `nBOOT0=0` → bootloader, `nBOOT0=1` → flash.) On this board the
default OPTR is `0x1feff8aa` (nSWBOOT0=1 → BOOT0 from pin, pin floats high → bootloader); the
target is `0x1beff8aa`.
```
# openocd offset 0x40 == FLASH_OPTR (0x40022040); verify it reads 0x1feff8aa first.
$GDB -batch -ex "target extended-remote localhost:3333" -ex "monitor halt" \
  -ex "monitor stm32l4x unlock 0" \
  -ex "monitor stm32l4x option_write 0 0x40 0x1beff8aa 0x0c000000" \
  -ex "monitor stm32l4x option_load 0" -ex quit
# option_load resets the chip; "option load failed / Protocol error" at the end is just gdb
# losing the target during that reset — it DID apply. Re-read VTOR to confirm 0x08000000.
```
The masked write (`mask 0x0c000000`) touches ONLY bits 26/27 — **never the RDP byte (0xAA)**.
Safety net: **SWD works in any boot mode**, so a wrong option write is always recoverable by
reconnecting and rewriting; the only true brick is RDP level 2, which we never touch.

After this the M33 boots commander from flash on every power-up, decoupled from the Arduino
orchestration — exactly what we want (commander is a bare firmware). Trade-off: this disables
the BOOT0-pin DFU entry the stock Arduino sketch-flash uses; we flash via SWD, so it's moot,
and it's reversible (`option_write` back to `0x1feff8aa`).

### openocd gotchas (cost us hours)
- **Run ONE `arduino-debug` at a time.** Multiple openocd instances collide on the SWD; a
  stuck **root-owned** openocd holds the SWD and can leave the CPU **halted** (looks identical
  to "firmware dead"). `adb shell` is user `arduino` — kill a root openocd with
  `echo <pw> | sudo -S pkill -9 -f openocd` before starting fresh.
- `west flash` for `arduino_uno_q` is **not working** upstream yet — the gdb-load recipe above
  is the method (openocd is the *debug* runner; stm32cubeprogrammer the nominal flash one).

### CONSOLE READ — the open problem (decides step 2)
adb shell runs as user **arduino (uid 1000), NO passwordless sudo**. `/dev/ttyHS1` (the
QRB↔MCU bridge, 115200) is held by **arduino-router.service** (systemd, RPC over MsgPack).
The router proxies its Monitor to `/dev/ttyGS0` → host **`/dev/cu.usbmodem16808266742`**, but
a **stock Zephyr `usart1` console does NOT appear there** (router only forwards framed RPC).
So to see console output, pick one for step 2:
- **(A) RTT console** — build `-DCONFIG_USE_SEGGER_RTT=y -DCONFIG_RTT_CONSOLE=y
  -DCONFIG_UART_CONSOLE=n`, read via openocd RTT (needs an extra adb-forward for the RTT TCP
  port). Sudo-free, deterministic over the SWD we already drive. Good for spike iteration.
- **(B) Stop the router + read `ttyHS1` raw** — `sudo systemctl stop arduino-router
  arduino-router-serial` (needs the user's password, one-time via `! ...`), then
  `cat /dev/ttyHS1`. This is the REAL target path (commander↔Debian over the bridge UART),
  IF usart1 is physically wired to the QRB bridge (UNVERIFIED — could be a different UART).
- **(C)** Make commander speak Arduino's MsgPack RPC over the bridge so the stock router/
  Monitor shows it — most "native" but most work; revisit later.
Recommended: do **(A) RTT** to finish step 1 + prototype commander in step 2 quickly; settle
(B)/(C) when wiring commander to the real Debian-side link.

## Prerequisites (one-time env)
- Install the **Zephyr SDK** + **west**; create a west workspace (`west init` / `west update`).
- **Board:** upstream Zephyr has `boards/arduino/uno_q`. Pin to a Zephyr release that
  includes it (verify the board name string — likely `arduino/uno_q`). If only on `main`,
  use that.
- **Flashing reality (from prior research):** the U585's SWD is NOT on a header. The QRB2210
  (Debian) runs an on-board OpenOCD with internal SWD; reach it via `arduino/remoteocd`
  (Local / ADB-over-USB / SSH) or `adb forward tcp:3333 tcp:3333` + `arduino-debug`, then
  openocd/gdb to `localhost:3333`. Arbitrary `.elf` works (not Arduino-locked).
- **Bridge UART:** MCU `Serial1` ↔ Linux `/dev/ttyHS1` @ 115200 (plus an internal SPI bus).
  This is where commander's console will live so Debian can drive it.
- **Reference:** the Zephyr `arduino/uno_q` **devicetree** is the source of truth for clocks
  (160 MHz), *which* USART is the bridge + its pins, the SPI map, and the flash runner.

## Step 1 — toolchain + flash, ZERO commander
De-risk the environment before writing any commander code. If this fights us, we've learned
the answer cheaply.
1. `west build -b arduino/uno_q zephyr/samples/hello_world`
2. Flash via the on-board OpenOCD path (adb-forward + the board's openocd runner, or
   `remoteocd`). **Nail the exact `west flash` incantation here and record it below** — this
   is the single most likely snag (there are reports of Zephyr-flash friction on this board).
3. Confirm `Hello World` on the bridge UART (from Debian: `screen /dev/ttyHS1 115200`, or
   pyserial).
4. Build + flash `zephyr/samples/subsys/shell/shell_module` → confirm Zephyr's own shell
   responds over the same UART (validates console routing end-to-end).

**Success:** hello_world prints and the Zephyr shell echoes over `/dev/ttyHS1`.

### Flash recipe (researched on the live board 2026-06-12)
Board Linux side = **Debian 13 (trixie)** on the QRB2210, reachable over `adb` (the board
shows up in `adb devices`). The on-board OpenOCD bit-bangs SWD to the U585 over GPIO:
- `/opt/openocd/bin/arduino-debug.sh` + `/opt/openocd/{stm32u5x,stm32x5x_common,openocd_gpiod}.cfg`
- front-end `/usr/local/bin/arduino-debug`; `arduino-cli` w/ the `arduino:zephyr:unoq` core present.
- `openocd` is NOT on PATH and nothing listens on :3333 until you start arduino-debug.

**`west flash` is NOT integrated for this board** (per Zephyr board doc) — use `west debug`
(gdb `load` writes flash). Recipe:
```
# Terminal A — on-board SWD/openocd gdb server, forwarded to localhost:3333:
adb forward tcp:3333 tcp:3333 && adb shell arduino-debug      # leave running
# Terminal B — build, then load+run:
~/zephyrproject/.venv/bin/west build -b arduino_uno_q <sample-path>
~/zephyrproject/.venv/bin/west debug -r openocd               # gdb connects :3333, load, run
```
For non-interactive load (flash-and-go) once confirmed: run the SDK gdb in batch —
`arm-zephyr-eabi-gdb build/zephyr/zephyr.elf -ex "target extended-remote :3333" -ex load
-ex detach -ex quit` — or try Arduino's `remoteocd` wrapper. Bootloader restore (if ever
needed): `arduino-cli burn-bootloader -b arduino:zephyr:unoq -P jlink`.
TODO at the bench: confirm board name is `arduino_uno_q` (vs `arduino/uno_q`) via
`west boards | grep uno`, and which DT node is the console/bridge UART (= /dev/ttyHS1).

**Env (this machine):** macOS 15.6 x86_64; west v1.5.0 in `~/zephyrproject/.venv`; build deps
(cmake/ninja/dtc/gperf) present; adb 1.0.41; Uno Q connected (adb id 1680826674).
Zephyr workspace at `~/zephyrproject`, zephyr = **v4.4.99 main** (board `arduino_uno_q`,
console = `usart1` PB6/PB7; lpuart1 also present w/ RTS/CTS).

### TOOLCHAIN GOTCHA (Intel Mac) — solved
Zephyr main pins **SDK 1.0.1**, whose macOS assets are **aarch64-only** — there is **NO
macОS x86_64 (Intel) toolchain in SDK 1.0.x** (`west sdk install -t arm-zephyr-eabi` → empty
list / "not available"). Fix: **skip the Zephyr SDK, use the `gnuarmemb` variant** with the
already-installed Arm GNU Toolchain. Every west build/flash for this board needs:
```
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi
```
Confirmed: `west build -p always -b arduino_uno_q samples/hello_world` → builds clean
(zephyr.elf, FLASH 0.88% of 2MB). [If host tools are ever needed beyond cmake/ninja/dtc/
gperf, note SDK 1.0.x has no Intel-mac host-tools bundle either — would force a stable
Zephyr release on the 0.17.x SDK, IF that release has the uno_q board.]

## Step 2 — minimal commander → `help`
Bring up commander's core on Zephyr with the HAL seam intact. Prototype in a **scratch Zephyr
app** first; promote `hal/zephyr/` + `runners/zephyr/` into the commander repo once `help`
works.

New code:
- **`hal/zephyr/hal.cpp`** — implement `hal.h` over Zephyr drivers. For the spike only need:
  - UART: `uart_poll_in` / `uart_poll_out` (maps cleanly to `hal_uart_getchar/putchar/puts`).
  - time: `k_msleep` (`hal_delay_ms`), `k_cycle_get_32`→µs (`hal_time_us`).
  - GPIO: `gpio_pin_configure/set/get` (can stub until a real module needs it).
  - I2C: stub for now; later `i2c_read` (= `hal_i2c_read_raw`), `i2c_write` / `i2c_write_read`.
  - **Devicetree note:** Zephyr addresses peripherals by DT device handles, not pin numbers.
    The HAL grabs `DEVICE_DT_GET(...)`; `hal_*_init` pin args become advisory/ignored, and
    board pin/bus config lives in a `.overlay`. (This is the one real seam adaptation.)
  - Route commander's UART to the bridge UART: `DEVICE_DT_GET(DT_CHOSEN(zephyr_console))`
    (or the specific bridge USART nodelabel) so `help` comes out on `/dev/ttyHS1`.
- **`runners/zephyr/`** — a Zephyr app: define `int main(void)` that brings up the console
  UART, registers `SystemModule`, and starts the UART transport thread, then returns (the
  kernel keeps the thread running). Reuses `CommandRegistry`, `UartTransport`, `SystemModule`
  **unchanged**.
  - Threading glue: `UartTransport::taskBody(void*)` is already a static, xTaskCreate-shaped
    entry. Wrap it: `K_THREAD_STACK_DEFINE(stack, 2048)` + `k_thread_create(&t, stack,
    sizeof(stack), (k_thread_entry_t)UartTransport::taskBody, &uart, NULL, NULL, prio, 0,
    K_NO_WAIT);`
- **`prj.conf`** essentials: `CONFIG_CPP=y`, `CONFIG_STD_CPP17=y`, `CONFIG_CPP_EXCEPTIONS=n`,
  `CONFIG_SERIAL=y`, `CONFIG_GPIO=y`, (`CONFIG_I2C=y` later), a real libc
  (`CONFIG_NEWLIB_LIBC=y` or picolibc) for `snprintf`, and a sane `CONFIG_MAIN_STACK_SIZE`.
  commander's core is heap-free (static registry), so no big heap pool needed for the spike.
- **`.overlay`** — only if the bridge UART/pins need binding beyond the stock board DT.

**Success:** from Debian, open `/dev/ttyHS1` @ 115200, type `help`, get `SystemModule`'s
output. That's the milestone — commander's core running on Zephyr, seam intact.

Then (still step 2, stretch): flesh out the I2C HAL and bring up one real module (e.g.
`i2c`-diag or `compass`) to prove a HAL-only module works unmodified. Watch for `std::vector`
usage in modules → needs `CONFIG_LIB_CPLUSPLUS` + full libc/libstdc++; SystemModule itself is
clean.

## Key risks / gotchas (in likely-to-bite order)
1. **`west flash` on the Uno Q** — the OpenOCD-over-adb path is the #1 snag. Solve in Step 1.
2. **Board availability / Zephyr version** — confirm `arduino/uno_q` is in the chosen release.
3. **C++ under Zephyr** — fine for SystemModule (careful C++, no exceptions); STL-using
   modules need the right Kconfig later.
4. **Console routing** — making commander's UART == the bridge UART so Debian sees `help`.

## After the spike (OUT OF SCOPE now)
- Promote `hal/zephyr/` + `runners/zephyr/` into commander proper.
- A `cmdr` target for Zephyr boards (init + module emit), so the cmdr flow extends.
- Decide per-board: Zephyr backend only where it's *capable + no native port + want the
  subsystems + Zephyr has the board* (Uno Q = all four). Don't migrate working native ports.
