# Getting started — Arduino Uno Q (without App Lab)

> **Status: captured empirically.** The fresh-image facts in §1 were recorded after an
> actual stock reflash (2026-06-19), not inferred. One thing remains unverified: the stock
> `arduino` password *value* — we know only that it ships expired, and you supply the
> current one when resetting it.

The Uno Q is a **dual-brain board**: a Qualcomm QRB2210 running Debian (the "SBC") and an
STM32U585 M33 running commander (Zephyr). They don't share storage. This guide brings up a
**stock-flashed board into a commander host *without* Arduino App Lab** — the GUI path most
docs assume.

The thing App Lab normally does that this guide must replace: **initial provisioning —
Wi-Fi and device naming** (per Arduino's UNO Q User Manual), and the Linux user credentials.
There is **no auto-setup triggered by `adb shell`** and no first-boot wizard on the board;
App Lab pushes the provisioning over USB. So a fresh image is reachable but unprovisioned.

The generic board tooling lives in the companion
**[unoq-tools](https://github.com/gbryant/unoq-tools)** repo: `setup-board.py` is the
interactive wizard for the provisioning this guide covers (password, hostname, ssh,
mDNS, Wi-Fi, headless slim), plus TTS and Bluetooth-audio setup and their docs
(headless trim, factory restore, ML backend, BT audio).

Companion docs in this repo (assume the post-provisioning state this guide reaches):
[`unoq-access.md`](./unoq-access.md) (access map + console bridge),
[`zephyr-hal-spike.md`](./zephyr-hal-spike.md) (M33 boot-byte write),
[`dev/unoq/README.md`](../dev/unoq/README.md) (Debian-side bridge/broker services).

---

## 0. Reflashing a used board? Save its state first

**Fresh board out of the box: skip straight to §1.** This section only matters if
you're about to reflash a board you've been using — the stock reflash wipes the
eMMC, so pull off anything that exists only on the board:

- **Provisioned facts** (what's set now, so re-provisioning is a checklist):
  ```bash
  adb shell 'whoami; id; hostname; head -2 /etc/os-release'
  adb shell 'nmcli -t -f NAME,TYPE,DEVICE connection show'
  adb shell 'ls -la /etc/NetworkManager/system-connections/'
  ```
- **Wi-Fi connection profile(s)** — so re-provisioning is a copy-back:
  ```bash
  adb pull /etc/NetworkManager/system-connections/   ./unoq-backup/system-connections/   # needs root/sudo
  ```
- **Home directory** — see §0.1.
- **Uncommitted edits** in any working directories that live on the board.

### 0.1 Back up the home directory
_(size check: run `adb shell 'du -sh /home/arduino'`; if modest, pull it whole)_

```bash
adb pull /home/arduino ./unoq-backup/home-arduino/
```

---

## 1. Fresh-image default state (confirmed 2026-06-19, after a stock reflash)

What a freshly flashed stock image actually gives you:

- **`adb` over USB: auth-free, lands as `arduino` (uid 1000).** The user is baked into the
  image — App Lab does NOT create it. Nothing auto-runs on `adb shell`; there is no first-boot wizard.
- **BUT the `arduino` account password ships EXPIRED.** It must be changed on first use, and
  **`sudo` is blocked until you reset it** (`sudo: Account or password is expired`). This is the
  silent first-boot gate App Lab handled. `adb` itself needs no auth, and `hostnamectl` works (it
  goes through polkit, not sudo) — but ssh/avahi/Wi-Fi/slim all need sudo, so reset the password
  first: `passwd` (over `adb shell -t`), or `setup-board.py --reset-password`. (We have NOT
  confirmed the stock password *value* — only that it ships expired; you supply it to reset.)
- **Hostname: `uno-q`** (the stock default — a custom name is something you set).
- **No Wi-Fi.** `wlan0` exists but is disconnected with no saved connection — Wi-Fi is the one
  real provisioning step App Lab did.
- **`ssh` and `avahi-daemon` (mDNS) are both DISABLED.** With no Wi-Fi + ssh off + no mDNS,
  **`adb`-over-USB is your only door** until you enable them — so do all provisioning over `adb` first.
- **No sshd host keys.** The stock image ships `/etc/ssh/` with no host keys, so simply enabling
  ssh fails to start (`sshd: no hostkeys available -- exiting`). You must `ssh-keygen -A` first —
  the wizard does this automatically, but note it if enabling ssh by hand. After a reflash the
  board's host keys are new, so if you SSH'd to the same hostname before you'll get a
  "host key has changed" / "Host key verification failed" warning on your Mac — clear the stale
  entry with `ssh-keygen -R <hostname>.local`, then accept the new fingerprint.
- **The full Arduino stack is live again** — `arduino-router`(+`-serial`) owns `/dev/ttyHS1`;
  `arduino-app-cli` + `docker`/`containerd` running; `lightdm` / `graphical.target` (GUI);
  `bluetooth`; `ModemManager`. Every systemd change a lean setup makes is reverted by the reflash
  (they lived on the wiped eMMC) — so "revert to stock" really is just "reflash."
- **Home is ~605 MB, almost all `.arduino15`** (which ships with the image). All genuinely user
  data (sketches, scripts, `piper_project`, `maps`, configs) is gone — back it up first (§0).
- **The M33 is untouched.** The eMMC reflash cannot reach the STM32, so its option bytes + last
  firmware persist. If you never ran the SWD boot-byte revert, the M33 still boots your commander
  firmware under a stock Debian (verify via VTOR — `docs/zephyr-hal-spike.md`).

---

## 2. Provisioning + slim — one host-side wizard

`setup-board.py` (from [unoq-tools](https://github.com/gbryant/unoq-tools)) does the whole setup **from your machine over adb** — no hand-piping
commands to the board. Just run it; it's an interactive wizard, no flags needed:

```bash
unoq-tools/setup-board.py
```

It inspects each thing, shows the current state, and asks permission before changing anything —
so it's **idempotent and safe to re-run** (already-done steps show ✓ and are skipped). The steps:

1. **Password** — if the account is expired (§1), runs `passwd` interactively (it asks you for the
   current password and the new one — the value isn't assumed/stored by the script).
2. **Hostname** — shows the current one, offers to change it.
3. **ssh** + **mDNS (avahi)** — enables each only if not already active.
4. **Wi-Fi** — if not connected, prompts for SSID + password and joins via `nmcli`.
5. **Slim** — if not already a headless host, offers to drop the GUI + ModemManager/bluetooth and
   mask the App Lab/Docker stack (~210 MB RAM; reverts in unoq-tools [unoq-linux-setup.md](https://github.com/gbryant/unoq-tools/blob/main/docs/unoq-linux-setup.md)).
6. **Reboot** — offered at the end.

It does NOT mask the router or install the bridge — that's §4 (`install_broker.sh`). Once Wi-Fi is
up, `ssh arduino@<hostname>.local` works.

---

## 3. MCU boot bytes (commander on the M33)

The reflash writes only the QRB2210 eMMC; the STM32 option bytes are untouched by it (see unoq-tools
[`unoq-factory-restore.md`](https://github.com/gbryant/unoq-tools/blob/main/docs/unoq-factory-restore.md)). To run commander on the M33 you need
it booting from flash instead of its ROM bootloader.

**You don't have to do this by hand.** Any scaffolded project ships `./enable-flash-boot`,
which does the option-byte write, is idempotent, and checks VTOR afterwards — so the usual
answer is "run that once, from your first project". The manual SWD procedure and the reasoning
behind the polarity are in [`zephyr-hal-spike.md`](./zephyr-hal-spike.md) → "Make the M33 boot
from flash".

- [ ] M33 boots from flash (option bytes `0x1feff8aa → 0x1beff8aa`).

---

## 4. Free the serial link + install the bridge/broker

The stock `arduino-router` stack owns `/dev/ttyHS1` and reclaims it on every boot, so commander
can't have the link until the router is **stopped and masked** — both, in that order. Masking
alone only blocks the next start; a router still running keeps its exclusive hold and whatever
you install crash-loops on `Device or resource busy`.

**You don't have to do this by hand either.** A scaffolded project's `./install-broker` does the
whole sequence and verifies the broker came up. Only one of these may own the link at a time:

- **broker** (`commander-broker.service`) — the channel bus: ch0 console **plus** per-channel
  sockets. What you want unless you have a reason not to.
- **bridge** (`commander-bridge.service`) — a plain `socat` console, no channels. The simpler
  fallback.

Background and the manual recipe: [`unoq-access.md`](./unoq-access.md) Phase 1 +
[`dev/unoq/README.md`](../dev/unoq/README.md).

- [ ] `arduino-router` stack stopped **and** masked.
- [ ] `commander-broker.service` **or** `commander-bridge.service` installed + enabled (not both).

---

## 5. (Optional) Headless trim

For a lean commander host, drop the GUI and unused daemons — see unoq-tools
[`unoq-linux-setup.md`](https://github.com/gbryant/unoq-tools/blob/main/docs/unoq-linux-setup.md) (frees ~210 MB RAM). The App Lab + Docker
stack must be **masked, not disabled** (it socket-activates back otherwise).

---

## 6. Verify

- [ ] `ssh`/`adb` shell reachable, hostname resolves.
- [ ] MCU console over the bridge (Mac `/dev/cu.usbmodem*`) — `help` responds.
- [ ] (broker build) a channel publishes MCU→Debian while ch0 console works — see
      [`commander-channels-bringup.md`](./commander-channels-bringup.md).

---

## 7. Next — build something on it

[`unoq-ir-speaker.md`](./unoq-ir-speaker.md) takes a provisioned board through a complete
project: scaffold, flash, broker, and a remote whose button names are spoken aloud — no code
written. It's the fastest way to prove the whole dual-brain path works, and its
layer-by-layer troubleshooting section is the reference when something in your own project
goes quiet.
