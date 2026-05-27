"""
Patch FreeRTOS library config for Arduino Uno (ATmega328P).

Problems with the library defaults on a 2KB-RAM board:
  configUSE_TIMERS=1      — timer task + queue consume ~480 bytes of heap at
                            scheduler start, starving the idle task allocation.
  configMINIMAL_STACK_SIZE=192 — idle task only calls loop() (empty); 128 is ample.

Applied automatically before each build via extra_scripts; idempotent.
"""
Import("env")
import os
import re

config = os.path.join(".pio", "libdeps", env["PIOENV"], "FreeRTOS", "src", "FreeRTOSConfig.h")

PATCHES = [
    ("configUSE_TIMERS",             "1",   "0",   "timer task exhausts heap on Uno"),
    ("configMINIMAL_STACK_SIZE",     "192", "128", "idle task is trivially shallow"),
    ("configSUPPORT_STATIC_ALLOCATION", "0", "1",  "allow xTaskCreateStatic for UART task"),
]

if not os.path.exists(config):
    print("[patch] FreeRTOSConfig.h not found — run 'pio pkg install' first")
else:
    with open(config, "r") as f:
        src = f.read()

    for define, old_val, new_val, reason in PATCHES:
        pattern = rf"(#define\s+{re.escape(define)}\s+){re.escape(old_val)}\b"
        new_src, count = re.subn(pattern, rf"\g<1>{new_val}", src)
        if count:
            src = new_src
            print(f"[patch] FreeRTOSConfig.h: {define} {old_val} → {new_val} ({reason})")

    with open(config, "w") as f:
        f.write(src)
