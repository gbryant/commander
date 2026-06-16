"""Golden-file codegen tests — the highest-leverage cmdr regression net.

For a matrix of (target, module-set) we call generate_modules_file() and assert the
emitted commander_modules.h matches a checked-in snapshot under golden/. An
*unexpected* diff is the regression signal (a duplicate include, a mis-wired hook,
a dropped dedup). Snapshots are updated deliberately:

    CMDR_UPDATE_GOLDEN=1 python3 -m pytest tools/cmdr/tests/test_codegen_golden.py

The goldens double as living documentation of exactly what each module emits.
"""
import os

import pytest

from conftest import GOLDEN_DIR, build_modules

# Each case: (target, label, [module names], {per-module opt overrides}).
# Chosen to cover the shipped configs + the wiring that has bitten:
#   • the I2C dedup (compass+i2c+locomotion share one hal_i2c_init)
#   • the unoq channel-bus IR hook (vs the UART hook everywhere else)
#   • aicam's uart vs i2c transport branch
#   • esp32 display stack (ipstube+ws2812+ds1302) with its on_*_ready hooks
MATRIX = [
    ("uno",      "base",            [], {}),
    ("uno",      "ir",              ["ir"], {}),
    ("uno",      "compass_sonar",   ["compass", "sonar"], {}),
    ("r4",       "base",            [], {}),
    ("r4",       "wifi_ir",         ["wifi", "ir"], {}),
    ("r4",       "loco_bridge",     ["loco-bridge"], {}),
    ("r4",       "roomba",          ["roomba"], {}),
    ("pico",     "base",            [], {}),
    ("pico",     "ir",              ["ir"], {}),
    ("pico",     "ir_wall",         ["ir"], {"ir": {"wall": True}}),
    ("pico",     "controller",      ["controller"], {}),
    ("pico",     "i2c_dedup",       ["compass", "i2c", "locomotion"], {}),
    ("pico",     "wifi_ctrl_loco",  ["wifi", "controller", "locomotion"], {}),
    ("pico2",    "controller_loco", ["controller", "locomotion"], {}),
    ("esp32",    "base",            [], {}),
    ("esp32",    "wifi",            ["wifi"], {}),
    ("esp32",    "ina219",          ["ina219"], {}),
    ("esp32",    "display_stack",   ["ipstube", "ws2812", "ds1302"], {}),
    ("esp32",    "aicam_uart",      ["aicam"], {}),
    ("esp32",    "aicam_i2c",       ["aicam"], {"aicam": {"transport": "i2c"}}),
    ("bluepill", "base",            [], {}),
    ("bluepill", "compass_ds1302",  ["compass", "ds1302"], {}),
    ("unoq",     "base",            [], {}),
    ("unoq",     "ir",              ["ir"], {}),
]

UPDATE = os.environ.get("CMDR_UPDATE_GOLDEN") == "1"


def _ids(case):
    return f"{case[0]}-{case[1]}"


@pytest.mark.parametrize("case", MATRIX, ids=[_ids(c) for c in MATRIX])
def test_golden(case, cli_mod, tmp_path):
    target, label, names, overrides = case
    modules = build_modules(target, names, overrides)
    out = tmp_path / "commander_modules.h"
    cli_mod.generate_modules_file(target, modules, out)
    emitted = out.read_text()

    golden = GOLDEN_DIR / f"{target}__{label}.h"
    if UPDATE:
        GOLDEN_DIR.mkdir(exist_ok=True)
        golden.write_text(emitted)
        pytest.skip(f"updated golden {golden.name}")

    assert golden.exists(), (
        f"missing golden {golden.name} — run CMDR_UPDATE_GOLDEN=1 pytest to create it"
    )
    assert emitted == golden.read_text(), (
        f"{golden.name} drifted. If intended: CMDR_UPDATE_GOLDEN=1 python3 -m pytest "
        f"tools/cmdr/tests/test_codegen_golden.py"
    )
