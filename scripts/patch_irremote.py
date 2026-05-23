"""
Ensure IRremote rawbuf is a static embedded array (not heap-allocated).
Heap allocation of 100 bytes fails silently on Uno R3 with ~175 bytes free RAM,
breaking IR receive entirely. Static array keeps rawbuf in BSS where it belongs.

Applied automatically before each build; idempotent — won't re-patch.
"""
Import("env")
import os

base = os.path.join(".pio", "libdeps", "uno", "IRremote", "src")

# ── Patch 1: IRremoteInt.h ────────────────────────────────────────────────────
# Ensure rawbuf is a static array, not a pointer.
header = os.path.join(base, "IRremoteInt.h")
if os.path.exists(header):
    with open(header, "r") as f:
        src = f.read()
    ptr = "IRRawbufType *rawbuf;"
    arr = "IRRawbufType rawbuf[RAW_BUFFER_LENGTH];"
    if ptr in src:
        with open(header, "w") as f:
            f.write(src.replace(ptr, arr, 1))
        print("[patch] IRremoteInt.h: rawbuf heap pointer → static array")
    else:
        print("[patch] IRremoteInt.h: rawbuf already a static array, no change")

# ── Patch 2: IRReceive.hpp ────────────────────────────────────────────────────
# Remove any rawbuf heap allocation from begin() or constructor.
receive = os.path.join(base, "IRReceive.hpp")

ALLOC_BLOCK = (
    "    if (irparams.rawbuf == nullptr) {\n"
    "        irparams.rawbuf = new IRRawbufType[RAW_BUFFER_LENGTH];\n"
    "    }\n"
)

if os.path.exists(receive):
    with open(receive, "r") as f:
        src = f.read()
    if ALLOC_BLOCK in src:
        with open(receive, "w") as f:
            f.write(src.replace(ALLOC_BLOCK, "", 1))
        print("[patch] IRReceive.hpp: removed rawbuf heap allocation")
    else:
        print("[patch] IRReceive.hpp: no heap allocation found, no change")
