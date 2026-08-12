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
| **MCU / commander** | USB CDC (`ttyGS0`) ↔ `ttyHS1` ↔ `lpuart1` | `commander-broker.service` (channel bus; `commander-bridge.service` = plain-console fallback) | open `/dev/cu.usbmodem*` | ✅ after Phase 1 (below) |

Key distinction: **adb/ssh reach Debian (the SBC); the USB serial reaches the MCU
(commander).** They are different targets — don't conflate them.

> **Before any of this matters, the M33 has to actually *run* commander.** Out of the box it
> boots the STM32 ROM bootloader, not your flashed firmware, so the link is silent no matter
> how the bridge is set up. That's a **one-time STM32 option-byte write** (`nSWBOOT0=0`/
> `nBOOT0=1`), independent of the router masking below — masking the Arduino stack does NOT
> control MCU boot (verified). See **"Make the M33 boot from flash" in `zephyr-hal-spike.md`.**

## Phase 1 — durable commander access (DONE)

Problem: by default `arduino-router` owns `ttyHS1` (its MsgPack RPC) and reclaims it on
every boot, blocking commander's raw bridge. Fix = two systemd changes on the board:

1. **Mask the Arduino router stack** (frees `ttyHS1`; App Lab isn't used here anyway).
   `disable` is NOT enough — the router gets pulled back in three ways: the
   `arduino-router-serial.path` trigger (re-)starts `arduino-router-serial.service`, which
   `Requires=arduino-router.service`, and `arduino-app-cli.service` `Wants` it too. Only
   **masking** blocks all of those. The units are real files in `/etc/systemd/system`, so
   `systemctl mask` won't symlink over them — move the file aside first:
   **Stop BEFORE masking.** Masking only blocks a future start — a router already running keeps
   its *exclusive* hold on `ttyHS1`, so the bridge/broker dies on open() with `[Errno 16] Device
   or resource busy` and, with `Restart=always`, crash-loops silently until the next reboot.
   Stopping afterwards is worse than useless: once the unit file has been moved aside and masked,
   `systemctl stop <name>` may no longer find the unit to stop it, while the process lives on.
   ```
   sudo systemctl stop arduino-router-serial.path arduino-router-serial.service arduino-router.service
   sudo pkill -x arduino-router          # backstop; -x (exact name), NOT -f (see below)
   for u in arduino-router.service arduino-router-serial.service arduino-router-serial.path; do
     sudo mv /etc/systemd/system/$u /etc/systemd/system/$u.commander-bak   # free the path
     sudo ln -sf /dev/null /etc/systemd/system/$u                          # mask
   done
   sudo systemctl daemon-reload
   ```
   `pkill -x` matches the process *name*; `pkill -f /usr/bin/arduino-router` would match full
   command lines — including the shell running the command — so over `adb shell` it makes that
   shell kill itself and silently skip everything after it.

   `install_broker.sh` does all of this for you and then verifies the broker is `active`; this
   recipe is for doing it by hand or understanding what the script did.
   (`app-cli` only `Wants` the router, so it skips the masked unit cleanly; `ttyGS0` is
   created by adbd independently, untouched.)
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
for u in arduino-router.service arduino-router-serial.service arduino-router-serial.path; do
  sudo rm -f /etc/systemd/system/$u                                       # remove the /dev/null mask
  sudo mv /etc/systemd/system/$u.commander-bak /etc/systemd/system/$u      # restore the real unit
done
sudo systemctl daemon-reload
sudo systemctl enable --now arduino-router.service arduino-router-serial.service arduino-router-serial.path
```

> **needrestart note:** after `apt upgrade`, the "Daemons using outdated libraries" screen
> (`needrestart`) only *restarts running* services to pick up new libs — it does NOT change
> enable/disable/mask state. The mask above survives upgrades and reboots.

## Phase 2 — IP / multi-consumer (planned, requirements TBD)
Add a USB-ethernet gadget function (`usb_f_ncm`/`ecm` — modules present on this kernel) →
a `usb0` link to the Mac alongside WiFi → an SBC broker serves commander over **TCP** for
multiple consumers (SBC processes + host), per `docs/commander-channels-design.md`. The
output-tagging channel layer (the part that must live in commander) is the MCU-side
prerequisite. Not yet started.
