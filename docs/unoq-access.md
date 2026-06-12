# Arduino Uno Q — access map & reliable setup

How to reach the two brains on the Uno Q (Qualcomm QRB2210 running Debian, + an
STM32U585 running commander), in a way that survives reboots. Companion to
`docs/zephyr-hal-spike.md`. This board's hostname is `gandalf`; adjust IPs to yours.

## Access map

| You reach | Transport | Board endpoint | From your Mac | Durable? |
|-----------|-----------|----------------|---------------|----------|
| **Debian shell** | USB (ffs.adb gadget) | `adbd` | `adb shell` | ✅ adbd.service enabled |
| **Debian shell** | WiFi / IP | `sshd` | `ssh arduino@<ip>` (`gandalf.local`?) | ✅ ssh enabled + WiFi auto-connects |
| **Debian login** | UART `ttyMSM0` (QRB debug pins) | `serial-getty@ttyMSM0` | serial adapter on the debug header | ✅ enabled (needs physical UART) |
| **MCU / commander** | USB CDC (`ttyGS0`) ↔ `ttyHS1` ↔ `lpuart1` | `commander-bridge.service` (socat) | open `/dev/cu.usbmodem*` | ✅ after Phase 1 (below) |

Key distinction: **adb/ssh reach Debian (the SBC); the USB serial reaches the MCU
(commander).** They are different targets — don't conflate them.

## Phase 1 — durable commander access (DONE)

Problem: by default `arduino-router` owns `ttyHS1` (its MsgPack RPC) and reclaims it on
every boot, blocking commander's raw bridge. Fix = two systemd changes on the board:

1. **Disable the Arduino router** (frees `ttyHS1`; App Lab isn't used here anyway):
   ```
   sudo systemctl disable --now arduino-router.service arduino-router-serial.service
   ```
2. **Install a persistent bridge** that pipes the MCU UART to the *existing* USB CDC ACM
   (`ttyGS0`, part of the default adb+acm gadget — so adb is untouched, no gadget edit):

   `/etc/systemd/system/commander-bridge.service`:
   ```ini
   [Unit]
   Description=Commander MCU bridge (ttyHS1/lpuart1 <-> USB CDC ttyGS0)
   After=adbd.service

   [Service]
   ExecStart=/usr/bin/socat /dev/ttyHS1,b115200,raw,echo=0 /dev/ttyGS0,b115200,raw,echo=0
   Restart=always
   RestartSec=2

   [Install]
   WantedBy=multi-user.target
   ```
   ```
   sudo systemctl daemon-reload
   sudo systemctl enable --now commander-bridge.service
   ```

**Result:** `/dev/cu.usbmodem*` on the Mac *is* commander's console, on every boot.
Open it like any commander target:
```
screen /dev/cu.usbmodem* 115200      # or: python3 -m serial.tools.miniterm /dev/cu.usbmodem* 115200
```

### Flashing the MCU (commander firmware)
Independent of the bridge (SWD over adb), from `~/zephyrproject/cmdr-unoq-spike` etc.:
```
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/Applications/ArmGNUToolchain/14.2.rel1/arm-none-eabi
~/zephyrproject/.venv/bin/west build -b arduino_uno_q -d build .
adb forward tcp:3333 tcp:3333 && adb shell arduino-debug &      # on-board openocd
arm-none-eabi-gdb build/zephyr/zephyr.elf -batch \
  -ex "target extended-remote localhost:3333" -ex "monitor reset halt" -ex load \
  -ex "monitor reset run" -ex detach -ex quit
```
The Mac console (`cu.usbmodem`) shows the new firmware live — no need to detach the bridge.

### Revert to stock Arduino
```
sudo systemctl disable --now commander-bridge.service
sudo systemctl enable --now arduino-router.service arduino-router-serial.service
```

## Phase 2 — IP / multi-consumer (planned, requirements TBD)
Add a USB-ethernet gadget function (`usb_f_ncm`/`ecm` — modules present on this kernel) →
a `usb0` link to the Mac alongside WiFi → an SBC broker serves commander over **TCP** for
multiple consumers (SBC processes + host), per `docs/commander-channels-design.md`. The
output-tagging channel layer (the part that must live in commander) is the MCU-side
prerequisite. Not yet started.
