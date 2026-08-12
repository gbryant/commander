# Walkthrough — make the Uno Q speak your remote's buttons

Point a remote at the board, press a button, and hear its name. It's the shortest
end-to-end demonstration of what the Uno Q target is *for*: a microcontroller doing
hard-real-time work (decoding IR pulses) next to a Linux box doing something a
microcontroller can't (neural text-to-speech), with commander's channel bus between them.

You write **no code**. The IR decoding is a stock module, the speaking is a tool the
module ships, and the wiring between them is one `cmdr autostart` line.

Expect about half an hour on a board that's already provisioned, most of it waiting
for builds and a voice download.

---

## What you need

- An **Arduino Uno Q** with `adb` reachable over USB.
- An **IR receiver module** (TSOP-style, the kind in every Arduino kit) with its signal
  pin on **D5** — the scaffold default. Power it per its own datasheet.
- A **Bluetooth speaker**, and a **remote** to press.
- On your Mac/Linux host: `adb`, a Zephyr/`west` checkout (`~/zephyrproject`), the Arm
  GNU Toolchain, and `cmdr` ([getting-started.md](getting-started.md)).

**Assumed:** the board is provisioned — on Wi-Fi, hostname set, `adb shell` works. If
it's fresh from a stock image, do [getting-started-unoq.md](getting-started-unoq.md)
first; App Lab normally does that provisioning and this path skips App Lab entirely.

---

## 1. Board side — audio (once per board)

The Debian side does the talking, so give it a voice before the firmware exists. Both
steps use the companion [unoq-tools](https://github.com/gbryant/unoq-tools) repo, and
neither depends on commander:

```bash
cd ~/github/unoq-tools
./setup-bt-audio.py          # packages + the headless WirePlumber fixes, then pair
./bt.py connect <MAC>        # or just `./bt.py connect` and pick from the list
./setup-tts.py               # Piper via pipx + voices (a few minutes)
./tts.py daemon install      # keep a voice warm -> instant speech
./tts.py doctor              # every line should be a tick
```

`daemon install` also switches on a **keep-alive floor**, and it matters more than it
sounds: a Bluetooth speaker mutes its amplifier after a few seconds of silence and takes
about a second to wake, which eats the start of exactly the short utterances this demo
produces ("play", "pause"). The keep-alive streams a sub-audible noise floor so the amp
never sleeps. Toggle it with `tts.py keepalive on|off` — turn it off and you'll hear
button names start halfway through. Background:
[unoq-bluetooth-audio.md §3c](https://github.com/gbryant/unoq-tools/blob/main/docs/unoq-bluetooth-audio.md).

**The speaker does not auto-reconnect after a board reboot** — re-run `bt.py connect`.

---

## 2. Scaffold the project

```bash
cd ~/github
cmdr init unoq irboom
cd irboom
cmdr module enable ir            # NEC/Sony receive on D5, published on channel 1
cmdr autostart add "ir recv"     # stream presses from boot, with no command sent
```

`cmdr` only generates software. Everything that changes the board is a **script you run**,
each with a revert — that separation is deliberate, so nothing mutates your hardware behind
your back.

The autostart line is what makes this zero-code. `ir recv` is a toggle on the M33's own
shell; recording it as an autostart makes receiving a *standing capability* of the board, so
a fresh boot streams presses on channel 1 with nobody connected and the human console stays
free.

---

## 3. Build and flash

```bash
./build              # west build; also FetchContents the commander framework
./enable-flash-boot  # ONE TIME PER BOARD — see below
./flash              # openocd-over-adb gdb load  (./bum = build + flash + monitor)
```

**`./build` must come first**, before anything else here: it downloads the framework that
`install-broker` later runs from.

**`./enable-flash-boot` is the step people miss.** The Uno Q ships with its M33 booting the
STM32 ROM bootloader, not its own flash — Arduino's native model has the Linux side load
sketches into the MCU. So a bare commander firmware never runs and the link is *dead
silent*, which looks exactly like a wiring fault. The script writes two option bytes
(nSWBOOT0=0, nBOOT0=1), is idempotent, checks VTOR afterwards, never touches the read-protect
byte, and is reverted by `./restore-arduino`. It's **per board, not per project** — a second
project on the same board skips it.

---

## 4. Hand the MCU link to commander

```bash
./install-broker     # needs the board sudo password
```

`/dev/ttyHS1` is the UART between the two brains, and the stock `arduino-router` owns it.
This stops and masks the router stack and installs `commander-broker.service`, which
demultiplexes the link into the ch0 console (also exposed on your Mac as a USB serial port)
and per-channel sockets under `/tmp/commander/`.

It ends with `broker active ✓`. If it doesn't, it prints the log and stops — believe it.

For scripts and CI, pass the password instead of being prompted:

```bash
BOARD_SUDO_PW=... ./install-broker
```

The masking survives reboots, so this is once per board too.

---

## 5. Put the tools on the board

```bash
./deploy-sbc
```

`cmdr module enable ir` dropped four tools into `bin/` and seeded `maps/` with a library of
known remotes. These run **on the Debian side, next to the broker** — unlike the serial
boards, where IR tools run on your host. They are pure subscribers to `ch1.sock`: they start
nothing and can't disturb the stream.

---

## 6. Run it

```bash
adb shell "cd /home/arduino && \
  XDG_RUNTIME_DIR=/run/user/\$(id -u) \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/\$(id -u)/bus \
  python3 ir_speak.py"
```

The environment prefix is not optional: a one-shot `adb shell` is neither a login nor an
interactive shell, so it sources nothing and lands with no audio session.

Press buttons:

```
Loaded 7 maps  [349 entries]
Speaking matched names via the Piper TTS daemon (voice already warm).
listening on ch1 — Press any button.  Ctrl-C to quit.

  ✓ 🔊  Sony      0xf       0x2a    12b  →  sony_minidisc_recorder : play
  ✓ 🔊  Sony      0xf       0x28    12b  →  sony_minidisc_recorder : stop
  ✓ 🔊  Sony      0xf       0x29    12b  →  sony_minidisc_recorder : pause
```

Each line is also spoken aloud. That's the whole demo.

---

## 7. Your own remote

If your remote isn't in the seeded maps, build one — press each button as prompted and it
writes a JSON map:

```bash
adb shell "cd /home/arduino && python3 ir_map.py -o myremote.json"
adb pull /home/arduino/myremote.json maps/     # keep it under version control
```

Maps are plain JSON and interchangeable with the serial boards' format. `ir_lookup.py` is
`ir_speak.py` without the speaking, if you just want to watch.

---

## When it doesn't work — check the layers in order

Each rung tells you which layer is broken, so start at the top and stop at the first
surprise.

**1. Is the broker up?**
```bash
adb shell 'systemctl is-active commander-broker.service'   # want: active
adb shell 'ls /tmp/commander/'                             # want: ch0.sock  ch1.sock
```
`activating` means it's crash-looping. The usual cause is something else holding
`/dev/ttyHS1` (`journalctl -u commander-broker` will say `Device or resource busy`) — check
`adb shell 'ps aux | grep arduino-router'` and re-run `./install-broker`.

**2. Is commander alive on the M33?**
```python
# adb push this and run it on the board
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/commander/ch0.sock')
s.sendall(b'help')        # ONE WRITE = ONE COMMAND, no trailing newline
s.settimeout(4)
print(s.recv(4096).decode())
```
A command list back means the firmware is running and the boot bytes are set. Silence means
the M33 isn't running commander — you skipped `./enable-flash-boot`, or the flash didn't take.
(Sending `b'\nhelp\n'` returns `unknown:` — the socket is message-framed, not a terminal.)

**3. Is IR receiving?** `ir recv` on ch0 **toggles**, and reports the state it just moved to.
`stopped.` means it *was* running and you've now switched it off — send it again. If nothing
arrives on ch1 while it says `listening...`, suspect the receiver wiring or the D5 pin.

**4. Can the board speak?** `tts.py doctor` — it audits piper, voices, the daemon, the
Bluetooth connection, the default sink and the keep-alive, and prints the fix for each.

**5. Speech starts halfway through a word?** The keep-alive is off: `tts.py keepalive on`.

---

## Traps worth knowing

- **`adb` loses the board on reboot.** `adb kill-server && adb start-server`. If it still
  doesn't appear, it's USB — check the cable and that it's not behind a hub.
- **`br-connection-page-timeout`** from `bt.py connect` means the speaker is off or out of
  range. That's different from **`br-connection-profile-unavailable`**, which is the headless
  WirePlumber seat gotcha (see unoq-tools).
- **Adopting framework fixes** in an existing project: `cmdr pull` re-fetches commander;
  `cmdr update && cmdr regen` refreshes the generated scripts. `regen` never touches your
  source, `cmdr.toml`, or `CMakeLists.txt`.
- **Both board-mutating scripts need a terminal** for their password prompt, or
  `BOARD_SUDO_PW=...`.

---

## Putting the board back

```bash
./restore-arduino    # stops commander's services, restores + starts the Arduino router,
                     # and optionally reverts the M33 boot bytes for App Lab
```

---

## What just happened

```
remote → IR receiver (D5) → Zephyr GPIO-ISR decoder on the M33 → channel 1
       → /dev/ttyHS1 → broker → ch1.sock → ir_speak.py → Piper → Bluetooth speaker
```

The M33 half is a stock module: it decodes NEC and Sony pulse trains and publishes each
press on channel 1, while the ch0 console stays independently usable. The Debian half is an
ordinary Python subscriber to a Unix socket. Nothing in the middle knows about IR or speech
— it's the channel bus, so the same shape carries any module's data to any consumer.

To go further: [modules.md](modules.md) for what else can be enabled,
[channels-first-class.md](channels-first-class.md) for the bus itself, and
[writing-a-module.md](writing-a-module.md) to add your own.
