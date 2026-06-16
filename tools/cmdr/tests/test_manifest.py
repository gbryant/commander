"""cmdr.toml manifest round-trip — read_manifest/write_manifest are the hand-rolled
(dependency-free) parser for our controlled schema. Typed values must survive a
write→read cycle so an enable doesn't quietly mutate a neighbouring module's opts.
"""
import pytest


def test_roundtrip_preserves_types(cli_mod, tmp_path):
    target = "pico"
    modules = {
        "ir": {"gpio": 22, "wall": True},
        "locomotion": {"addr": 0x42, "sda": 6, "scl": 7},
        "ws2812": {"pin": 5, "count": 6, "order": "GRB", "wall": False},
    }
    p = tmp_path / "cmdr.toml"
    cli_mod.write_manifest(p, target, modules)
    rt, rmods = cli_mod.read_manifest(p)

    assert rt == target
    assert rmods == modules, "manifest round-trip changed values/types"
    # spot-check the types specifically (bool must not collapse to int 1/0)
    assert rmods["ir"]["wall"] is True
    assert rmods["ws2812"]["wall"] is False
    assert isinstance(rmods["locomotion"]["addr"], int) and rmods["locomotion"]["addr"] == 66


def test_hex_and_string_values(cli_mod, tmp_path):
    p = tmp_path / "cmdr.toml"
    cli_mod.write_manifest(p, "esp32", {"ina219": {"channels": "a:0x40,b:0x45"}, "aicam": {"transport": "uart"}})
    _t, mods = cli_mod.read_manifest(p)
    # a comma-list string stays a string (not parsed as int)
    assert mods["ina219"]["channels"] == "a:0x40,b:0x45"
    assert mods["aicam"]["transport"] == "uart"


def test_empty_modules_manifest(cli_mod, tmp_path):
    p = tmp_path / "cmdr.toml"
    cli_mod.write_manifest(p, "r4", {})
    t, mods = cli_mod.read_manifest(p)
    assert t == "r4" and mods == {}


def test_bool_written_as_toml_literal(cli_mod, tmp_path):
    """bool must serialize as true/false, not 1/0 (it round-trips, but humans read it)."""
    p = tmp_path / "cmdr.toml"
    cli_mod.write_manifest(p, "pico", {"ir": {"wall": True}})
    text = p.read_text()
    assert "wall = true" in text
    assert "wall = 1" not in text


def test_modules_sorted_in_file(cli_mod, tmp_path):
    p = tmp_path / "cmdr.toml"
    cli_mod.write_manifest(p, "pico", {"wifi": {}, "controller": {}, "ir": {"gpio": 22}})
    sections = [ln for ln in p.read_text().splitlines() if ln.startswith("[module.")]
    assert sections == sorted(sections), "module sections should be written sorted for stable diffs"
