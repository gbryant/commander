"""Stamp the firmware with an incrementing build number + timestamp.

Runs pre-build (extra_scripts). Bumps `.build_number` in the project root on every
build and injects it (plus a timestamp) into version.h's overridable BUILD_NUMBER /
BUILD_COMMIT fields, so the `version` command reports exactly which build is
running — the reliable way to confirm an OTA actually landed.

Because the injected defines change each build, the firmware is rebuilt in full;
that's intended — an OTA image should be a clean, uniquely-stamped build.
"""
Import("env")  # noqa: F821  (provided by PlatformIO/SCons)
import datetime

COUNTER = ".build_number"
try:
    n = int((open(COUNTER).read().strip() or "0"))
except (FileNotFoundError, ValueError):
    n = 0
n += 1
with open(COUNTER, "w") as f:
    f.write("%d\n" % n)

stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
env.Append(CPPDEFINES=[
    ("BUILD_NUMBER", n),
    ("BUILD_COMMIT", env.StringifyMacro(stamp)),
])
print("[version] build #%d (%s)" % (n, stamp))
