#!/bin/bash
# The real `install-broker` logic, shipped WITH the commander framework. A project's
# `install-broker` is a thin shim that locates this file under the fetched commander source
# (build-unoq/_deps/commander-src/dev/unoq/) and execs it — so this logic updates with the
# framework (via `cmdr pull` / `cmdr clean` + build) and never goes stale in a project.
#
# Makes commander-broker.service own the MCU link: ch0 console -> the Mac's USB serial,
# chN -> /tmp/commander/chN.sock. Masks the Arduino router stack (frees ttyHS1) and replaces
# commander-bridge. Reversible with ./restore-arduino. Needs the board sudo password.
set -e

# This script lives at <commander>/dev/unoq/; the broker + unit sit alongside it in the tree.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMMANDER="$(cd "$HERE/../.." && pwd)"
BROKER="$COMMANDER/transport/channels/broker/commander_broker.py"
SERVICE="$HERE/commander-broker.service"
if [ ! -f "$BROKER" ] || [ ! -f "$SERVICE" ]; then
  echo "broker/service not found in the commander source ($COMMANDER) — bad fetch?" >&2
  exit 1
fi

# One sudo for all privileged steps: a single hidden prompt, no re-prompt, no echo leak.
# $BOARD_SUDO_PW skips the prompt, so this is runnable from a script or an agent session that
# has no TTY. Without it and without a terminal, `read` hits EOF and `set -e` would abort with
# NO output at all — silent and baffling — so say what happened instead.
if [ -n "$BOARD_SUDO_PW" ]; then
  PW="$BOARD_SUDO_PW"
elif [ -t 0 ]; then
  read -s -p "Board (arduino@gandalf) sudo password: " PW; echo
else
  echo "no terminal to prompt for the board sudo password." >&2
  echo "run this from a terminal, or pass it in:  BOARD_SUDO_PW=... ./install-broker" >&2
  exit 1
fi

echo "==> pushing broker + service unit to the board"
adb push "$BROKER"  /home/arduino/commander_broker.py      >/dev/null
adb push "$SERVICE" /home/arduino/commander-broker.service >/dev/null

echo "==> stopping + masking the Arduino router stack, installing commander-broker.service (one sudo)"
# STOP before mask, and pkill as the backstop. Masking only blocks a future start: a router
# already running keeps its EXCLUSIVE hold on /dev/ttyHS1, so the broker dies on open() with
# "[Errno 16] Device or resource busy" and Restart=always turns that into a silent crash-loop
# until the next reboot. The pkill also covers re-running this on a board masked by an older
# version of this script, where the units are masked (so `stop` may not find them) yet the
# original router process is still alive.
adb shell "echo '$PW' | sudo -S -p '' bash -c '
  systemctl stop arduino-router-serial.path arduino-router-serial.service arduino-router.service 2>/dev/null || true;
  pkill -f /usr/bin/arduino-router 2>/dev/null || true;
  cd /etc/systemd/system && for u in arduino-router.service arduino-router-serial.service arduino-router-serial.path; do [ -f \$u ] && [ ! -L \$u ] && mv \$u \$u.commander-bak && ln -sf /dev/null \$u; done; systemctl daemon-reload || true;
  cp /home/arduino/commander-broker.service /etc/systemd/system/ && systemctl daemon-reload && systemctl disable --now commander-bridge.service 2>/dev/null; systemctl enable commander-broker.service; systemctl restart commander-broker.service'"

# Verify for real. `is-active` straight after a restart reports "activating", which reads like
# success even when the broker is about to die on ttyHS1 — so settle first, then insist on
# "active" and show the log if it isn't.
sleep 3
STATE=$(adb shell "systemctl is-active commander-broker.service" 2>/dev/null | tr -d '\r')
if [ "$STATE" = "active" ]; then
  echo "broker active ✓ — open the Mac console with ./monitor"
else
  echo "broker is '$STATE', not active. Recent log:" >&2
  adb shell "journalctl -u commander-broker.service -n 15 --no-pager" >&2
  echo >&2
  echo "'Device or resource busy: /dev/ttyHS1' means something still owns the MCU link —" >&2
  echo "check with:  adb shell 'ps aux | grep arduino-router'" >&2
  exit 1
fi
