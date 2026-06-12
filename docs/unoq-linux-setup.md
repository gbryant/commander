# Arduino Uno Q — lean headless Linux setup (for a commander host)

The Uno Q's Qualcomm QRB2210 runs full Debian (with a desktop). For a headless
commander host you don't need the GUI or several stock daemons — trimming them frees
RAM/CPU on the 2 GB board. This is the system-tuning companion to `docs/unoq-access.md`
(which covers the access map + the commander console bridge). All commands assume the
`arduino` user (in `sudo`); apply over `adb shell` or `ssh`.

**Verified on:** Debian 13 (trixie), board `gandalf`. **Result:** RAM used 432 → 284 MB
(~150 MB freed); leaner still after a reboot (Xorg never starts).

## 1. Turn off the GUI (biggest win)
Boots to console instead of the graphical login; stops X/lightdm now.
```
sudo systemctl set-default multi-user.target
sudo systemctl disable --now lightdm.service
```
(Revert: `sudo systemctl set-default graphical.target && sudo systemctl enable --now lightdm`.)

## 2. Disable daemons a commander box doesn't use
```
sudo systemctl disable --now ModemManager.service   # no cellular modem on this board
sudo systemctl disable --now bluetooth.service       # MCU/commander doesn't use host BT
# docker = Arduino App Lab's container runtime. App Lab is masked (see unoq-access.md),
# so disable it IF you run no containers:
sudo systemctl disable --now docker.service docker.socket
```
(Each is reversible with `enable --now`. Check containers first: `sudo docker ps`.)

## 3. KEEP these — disabling them breaks access
| Service | Why keep |
|---|---|
| `NetworkManager` + `wpa_supplicant` | WiFi (your `ssh` / IP path) |
| `ssh` | network shell to Debian |
| `avahi-daemon` | mDNS → `gandalf.local` resolves; light |
| `adbd` | USB shell + also creates the `acm`/`ttyGS0` gadget the console rides |
| `serial-getty@ttyMSM0`, `getty@tty1` | login consoles (debug UART / local) |
| `commander-bridge` | the commander console over USB (`/dev/cu.usbmodem`) |

## 4. Also part of the headless commander setup (see unoq-access.md)
- **Mask the Arduino router stack** (`arduino-router` + `-serial` + the `.path`) so it can't
  reclaim `ttyHS1` — `disable` is NOT enough (a `.path` trigger + `app-cli` `Wants` pull it
  back). Full recipe + revert in `docs/unoq-access.md`.
- **`commander-bridge.service`** = `socat /dev/ttyHS1 ↔ /dev/ttyGS0` → commander appears as
  `/dev/cu.usbmodem` on the host. Also in `docs/unoq-access.md`.

## Notes
- **`needrestart`** (the "Daemons using outdated libraries" screen after `apt upgrade`) only
  *restarts running* services to pick up new libs — it does NOT re-enable/unmask anything.
  All of the above survive upgrades and reboots.
- **Reboot** to fully reclaim (some stopped-but-cached memory lingers until then) and to
  confirm the box comes up headless: `sudo reboot`.
- **Optional further trims** (small, desktop-helper daemons that mostly idle once the GUI is
  gone): `accounts-daemon`, `rtkit-daemon`, `colord`, `upower` — disable if you want the last
  few MB; not worth the risk if unsure.
