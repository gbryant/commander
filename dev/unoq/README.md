# Uno Q — Debian-side service units

systemd units that own the MCU↔Debian serial link on the Arduino Uno Q. They are
**alternatives, not co-runners** — both want `/dev/ttyHS1` + `/dev/ttyGS0`, and each is paired
with a different MCU firmware. Keep both in the repo so either can be (re)created later.

> The generic Uno Q host tooling that used to live here (board bring-up wizard,
> Piper TTS daemon + setup, Bluetooth audio, volume) moved to the freestanding
> [unoq-tools](https://github.com/gbryant/unoq-tools) repo — it isn't
> commander-specific. This directory keeps only what pairs with commander
> firmware: the two service units and `install_broker.sh` (the delegation
> target of a project's `install-broker` shim).

| Unit | Pairs with firmware | What it does |
|------|---------------------|--------------|
| `commander-bridge.service` | **plain UART console** (default build) | Dumb `socat` 1:1 passthrough `ttyHS1 ↔ ttyGS0`. The MCU's serial console appears on the Mac's `/dev/cu.usbmodem*`. No channels. |
| `commander-broker.service` | **channel bus** (`-DCOMMANDER_ENABLE_CHANNELS`) | Owns `ttyHS1`, demuxes the COBS channel bus: ch0 console → `ttyGS0` (Mac console, with echo/editing), other channels → `/tmp/commander/chN.sock` for local Debian consumers. |

Pick the one that matches what's flashed. The broker `Conflicts=` the bridge, so starting one
stops the other.

## Board paths (where things live on Debian)

| Thing | Path on the board |
|-------|-------------------|
| Unit files | `/etc/systemd/system/commander-{bridge,broker}.service` |
| Broker script | `/home/arduino/commander_broker.py` (source: `transport/channels/broker/commander_broker.py`) |
| Broker runtime (channel sockets) | `/tmp/commander/` (`ch0.sock`, `ch1.sock`, …) — ephemeral, recreated each start. Their presence is the "broker is healthy" signal |
| MCU link / USB-CDC gadget | `/dev/ttyHS1` (↔ `lpuart1` on the M33) / `/dev/ttyGS0` (↔ Mac `/dev/cu.usbmodem*`) |

## Prerequisites (both)

1. **MCU boots from flash.** Out of the box the M33 boots its ROM bootloader, so the link is
   silent regardless of which service runs. A scaffolded project's **`./enable-flash-boot`**
   does the one-time option-byte write; `docs/zephyr-hal-spike.md` → "Make the M33 boot from
   flash" explains what it writes and why.
2. **`ttyHS1` free of the Arduino router.** The router must be **stopped and masked** — both.
   **`./install-broker`** does that too; `docs/unoq-access.md` Phase 1 is the by-hand version.

## Install / switch

**The normal path is `./install-broker` from a scaffolded project.** It pushes the broker and
unit from the fetched framework source, stops and masks the router, enables the service, and
verifies it reached `active` — one sudo, no per-file copying, and no GitHub auth on the board
(so it works for a private repo). `BOARD_SUDO_PW=...` runs it without a prompt.

Everything below is the by-hand equivalent, for when you're working on the units themselves.
Note that copying and enabling is *not* sufficient on a board whose router still holds the
link — do the router step above first, or the service will crash-loop on
`[Errno 16] Device or resource busy`.

```bash
sudo cp dev/unoq/commander-broker.service /etc/systemd/system/
cp transport/channels/broker/commander_broker.py /home/arduino/commander_broker.py
sudo systemctl daemon-reload
```

**Use the broker (channel-bus firmware):**
```bash
sudo systemctl disable --now commander-bridge.service
sudo systemctl enable  --now commander-broker.service
systemctl is-active commander-broker.service   # want "active"; "activating" = crash-looping
```

**Switch back to the bridge (plain-console firmware):**
```bash
sudo systemctl disable --now commander-broker.service
sudo systemctl enable  --now commander-bridge.service
```

Updating the broker: re-copy `commander_broker.py` to `/home/arduino/`, then
`sudo systemctl restart commander-broker.service`.

## Revert to stock Arduino

A scaffolded project's **`./restore-arduino`** does the whole revert: stops and disables both
commander units, restores *and starts* the router stack (unmasking alone leaves the stock flow
dead until a reboot), and optionally writes the M33 boot bytes back to `0x1feff8aa`.

By hand: disable both units, **un-mask the `arduino-router` stack** and start it
(`docs/unoq-access.md` has the move-aside/symlink revert), and optionally `option_write` the
boot bytes back (`docs/zephyr-hal-spike.md`). Only one process may own `ttyHS1`, so the
commander units must be stopped *before* the router is started.
