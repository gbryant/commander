# Bluetooth Audio on the Arduino Uno Q (Debian) — Setup & Reconnect Guide

> Status: working as of 2026-06. Verified on the on-board Debian Trixie (aarch64),
> hostname `gandalf`, user `arduino`, accessed over `adb shell`.
> Audio server is **PulseAudio** (classic), **not** PipeWire.
> Test speaker MAC: `BC:87:FA:E2:D7:0D`.

---

## 0. TL;DR

1. **One-time, on a fresh image:** install the BlueZ + PulseAudio Bluetooth packages (§1),
   add `arduino` to the `bluetooth`/`audio` groups, then pair the speaker once (§2).
2. **Every new shell / after a disconnect:** export the session env vars and run the
   reconnect script (§3). That's the whole daily workflow.
3. **"Connected but silent"?** The speaker is almost always holding a stale Bluetooth
   link — **power-cycle the speaker first** (§5), then reconnect. Don't go chasing
   PulseAudio settings; the Linux side is usually fine.

---

## 1. One-time install (only needed on a fresh image)

These packages are the only thing you must reinstall after re-flashing. Everything else
in this guide is just commands you run; nothing else is installed.

```bash
sudo apt update
sudo apt install -y \
  bluez \
  pulseaudio \
  pulseaudio-module-bluetooth \
  libspa-0.2-bluetooth \
  espeak-ng       # optional: a quick way to test audio
```

> Note: your original notes also installed `pipewire-audio-client-libraries` and enabled
> PipeWire units. **You ended up on PulseAudio** (`pactl info` reports `Server Name: pulseaudio`),
> so you do *not* need PipeWire. If both stacks are installed they fight over the same
> socket — if you ever hit weird audio breakage, that's the first thing to check.

Add your user to the groups that are allowed to use Bluetooth and audio, then enable
"linger" so your user's services can run without an active login session (you're on `adb`,
not a normal desktop login):

```bash
sudo usermod -aG bluetooth,audio arduino
sudo loginctl enable-linger arduino
```

Log out / open a fresh `adb shell` after the `usermod` so the new groups take effect.

---

## 2. One-time pairing (only needed once per speaker)

Pairing is remembered across reboots (it's stored under `/var/lib/bluetooth`), so you only
do this the first time — or after a factory reset of the speaker.

First, set the session environment (explained in §3 — you need it for anything BT/audio):

```bash
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
```

Then pair interactively:

```bash
bluetoothctl
# --- now you're inside the bluetoothctl prompt ---
power on
agent on
default-agent
scan on                       # put the speaker in pairing mode; wait for its MAC to appear
pair  BC:87:FA:E2:D7:0D
trust BC:87:FA:E2:D7:0D        # 'trust' = auto-accept future reconnects
connect BC:87:FA:E2:D7:0D
quit
```

`scan on` prints a stream of nearby devices; find the line with your speaker's name and
note its MAC (the `AA:BB:CC:DD:EE:FF` value). Use that MAC everywhere below.

---

## 3. Daily reconnect (every new shell / after a disconnect)

Each new `adb shell` is a fresh session that doesn't know where PulseAudio's and BlueZ's
control sockets live, so you re-point it at them. This is why a new shell "loses" the audio
even though nothing actually broke.

Save this as `~/reconnect-bt-audio.sh` and `chmod +x` it:

```bash
#!/bin/bash
# Reconnect the Bluetooth speaker and route audio to it (PulseAudio).

# Point this session at the user's runtime + D-Bus sockets.
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus

# Persist those into the user systemd manager so services see them too.
systemctl --user import-environment XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS

MAC=BC:87:FA:E2:D7:0D

# Restart the Bluetooth and audio daemons clean, then connect.
sudo systemctl restart bluetooth
systemctl --user restart pulseaudio.service
sleep 2

bluetoothctl connect "$MAC"

# Test it.
sleep 1
espeak-ng "bluetooth audio ready"
```

Run it with:

```bash
~/reconnect-bt-audio.sh
```

If you hear "bluetooth audio ready," you're done. Then `aplay file.wav`, `espeak-ng "..."`,
Piper TTS, etc. all play over the speaker.

---

## 4. Quick health check (what "good" looks like)

```bash
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus

pactl info | grep "Server Name"     # -> pulseaudio
pactl list sinks short              # -> a 'bluez_sink.<MAC>.a2dp_sink' line should exist
pactl list cards short              # -> a 'bluez_card.<MAC>' line should exist
```

A healthy connected speaker looks like:

```
2  bluez_sink.BC_87_FA_E2_D7_0D.a2dp_sink  module-bluez5-device.c  s16le 2ch 44100Hz  SUSPENDED
```

- `a2dp_sink` in the name = the **high-quality** stereo profile. Good.
- `SUSPENDED` is **normal** when nothing is playing — PulseAudio parks idle sinks and wakes
  them on playback. It flips to `RUNNING` while audio actually plays. Not a problem.

If the `bluez_sink` line is **missing**, the speaker isn't truly connected (see §5).

---

## 5. Troubleshooting: "connected but silent"

This is the failure mode you hit most. `bluetoothctl` says `Connected: yes`, the
`bluez_sink` even shows up — but no sound.

### Most likely cause: the first sound after idle gets swallowed

Two things sleep when audio has been idle: **PulseAudio suspends the sink**
(`module-suspend-on-idle` — that's the `SUSPENDED` state), and **the speaker's amplifier
drops into standby**. The first playback after idle has to wake *both*, and the opening
burst of audio is eaten during that wake-up. You play something, hear nothing, and conclude
it's broken — then the next attempt works because everything is now warm. This explains why
power-cycling the speaker *seemed* to fix it: it was just forcing a fresh, awake transport,
not clearing a real fault.

**The reliable fix — stop the sink from suspending (lean on this if it keeps biting you):**

Edit `~/.config/pulse/default.pa` (create it if absent) and add:

```
.include /etc/pulse/default.pa
unload-module module-suspend-on-idle
```

Then restart PulseAudio:

```bash
systemctl --user restart pulseaudio.service
```

The sink stays awake, so playback is instant with no clipped first word. Tradeoff: the
speaker may not auto-sleep (minor power cost — fine for a dev setup). This matters
especially for Piper TTS, where you don't want the first word of every response clipped.

A lighter-weight alternative (no config change): send a throwaway "wake" burst before the
real audio in your scripts —

```bash
paplay /usr/share/sounds/alsa/Front_Center.wav 2>/dev/null   # wake-up burst
sleep 0.5
espeak-ng "the real message"
```

### If it's still silent after that — work the fix order

1. **Confirm the sink exists and goes RUNNING during playback:**
   ```bash
   pactl list sinks short          # is the bluez_sink listed?
   espeak-ng "test" &              # start audio...
   pactl list sinks short          # ...the bluez_sink should briefly show RUNNING
   ```
   If the sink is missing entirely (not just suspended), the speaker isn't really
   connected — re-run the reconnect script (§3), and as a last resort **power-cycle the
   speaker** to force a fresh transport.

2. **Only if it's still silent**, suspect Linux-side routing. Make the speaker the default
   and move any live streams onto it:
   ```bash
   pactl set-default-sink bluez_sink.BC_87_FA_E2_D7_0D.a2dp_sink
   for s in $(pactl list short sink-inputs | awk '{print $1}'); do
     pactl move-sink-input "$s" bluez_sink.BC_87_FA_E2_D7_0D.a2dp_sink
   done
   ```
   (In practice PulseAudio auto-promotes a freshly-connected A2DP sink to default, so you
   rarely need this — but it's the move if `pactl info | grep "Default Sink"` shows the
   built-in `Headphones` sink instead of the bluez one.)

### Other error you may see

```
Failed to connect: org.bluez.Error.Failed br-connection-profile-unavailable
```

Seen when connecting before the audio daemons were ready. The reconnect script (§3) avoids
it by restarting `bluetooth` + `pulseaudio.service` and `sleep`-ing before `connect`. If you
hit it manually, just re-run the connect a moment later.

---

## 6. Why the env vars are needed (background)

You're reaching the board over `adb shell`, which gives you a bare shell with **no login
session**, so two things that a normal desktop login sets up automatically are missing:

- `XDG_RUNTIME_DIR` — the per-user runtime dir (`/run/user/<uid>`) where PulseAudio and the
  user systemd manager keep their sockets.
- `DBUS_SESSION_BUS_ADDRESS` — the address of your user's D-Bus session bus, which
  `bluetoothctl`, `systemctl --user`, and PulseAudio all talk over.

Without them you get errors like:

```
Failed to connect to user scope bus via local transport:
$DBUS_SESSION_BUS_ADDRESS and $XDG_RUNTIME_DIR not defined
```

`enable-linger` (§1) keeps your user's runtime dir and services alive even with no login,
so these sockets exist for you to point at. Exporting the two vars (and `import-environment`
so services inherit them) is all that "re-connects" a new shell to the running audio stack —
nothing is actually broken, the shell just didn't know where to look.
