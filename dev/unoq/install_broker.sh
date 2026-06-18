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
read -s -p "Board (arduino@gandalf) sudo password: " PW; echo

echo "==> pushing broker + service unit to the board"
adb push "$BROKER"  /home/arduino/commander_broker.py      >/dev/null
adb push "$SERVICE" /home/arduino/commander-broker.service >/dev/null

echo "==> masking the Arduino router stack + installing commander-broker.service (one sudo)"
adb shell "echo '$PW' | sudo -S -p '' bash -c '
  cd /etc/systemd/system && for u in arduino-router.service arduino-router-serial.service arduino-router-serial.path; do [ -f \$u ] && [ ! -L \$u ] && mv \$u \$u.commander-bak && ln -sf /dev/null \$u; done; systemctl daemon-reload || true;
  cp /home/arduino/commander-broker.service /etc/systemd/system/ && systemctl daemon-reload && systemctl disable --now commander-bridge.service 2>/dev/null; systemctl enable commander-broker.service; systemctl restart commander-broker.service'"
adb shell "systemctl is-active commander-broker.service"     # status query — no sudo needed
echo "done. open the Mac console with ./monitor"
