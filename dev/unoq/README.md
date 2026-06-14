# Uno Q — Debian-side service units

systemd units that own the MCU↔Debian serial link on the Arduino Uno Q (`gandalf`). They are
**alternatives, not co-runners** — both want `/dev/ttyHS1` + `/dev/ttyGS0`, and each is paired
with a different MCU firmware. Keep both in the repo so either can be (re)created later.

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
| Broker runtime (PTY + sockets) | `/tmp/commander/` (`console`, `ch1.sock`, …) — ephemeral, recreated each start |
| MCU link / USB-CDC gadget | `/dev/ttyHS1` (↔ `lpuart1` on the M33) / `/dev/ttyGS0` (↔ Mac `/dev/cu.usbmodem*`) |

## Prerequisites (both)

1. **MCU boots from flash.** Out of the box the M33 boots its ROM bootloader — set the option
   bytes once: see `docs/zephyr-hal-spike.md` → "Make the M33 boot from flash". Without this
   the link is silent regardless of which service runs.
2. **`ttyHS1` free of the Arduino router.** Mask the `arduino-router` stack so it doesn't grab
   the port (see `docs/unoq-access.md` Phase 1).

## Install / switch (run on the board; needs sudo)

Copy a unit and the broker script (from a checkout of this repo, or `scp`/`adb push`):
```bash
sudo cp dev/unoq/commander-broker.service /etc/systemd/system/
cp transport/channels/broker/commander_broker.py /home/arduino/commander_broker.py
sudo systemctl daemon-reload
```

**Use the broker (channel-bus firmware):**
```bash
sudo systemctl disable --now commander-bridge.service
sudo systemctl enable  --now commander-broker.service
```

**Switch back to the bridge (plain-console firmware):**
```bash
sudo systemctl disable --now commander-broker.service
sudo systemctl enable  --now commander-bridge.service
```

Updating the broker: re-copy `commander_broker.py` to `/home/arduino/`, then
`sudo systemctl restart commander-broker.service`.

## Revert to stock Arduino

Both units are commander-specific; to hand the board back to Arduino's stack, disable them and
**un-mask the `arduino-router` stack** (`docs/unoq-access.md` has the move-aside/symlink revert),
and optionally `option_write` the boot bytes back to `0x1feff8aa` (`docs/zephyr-hal-spike.md`).
