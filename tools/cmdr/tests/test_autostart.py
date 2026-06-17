"""Autostart — `cmdr autostart` + the commander_run_autostart codegen.

Boot commands (cmdr.toml [autostart]) are dispatched once at startup for their side
effect (e.g. `ir recv` to start the IR stream with no command sent). These pin the
codegen (the generated function + NullWriter include + escaped dispatch lines), that
autostart survives a module enable (the manifest round-trip), and the CLI add/list/clear.
"""
from argparse import Namespace

import pytest


def test_codegen_emits_run_autostart(cli_mod, tmp_path):
    out = tmp_path / "commander_modules.h"
    cli_mod.generate_modules_file("unoq", {}, out, autostart=["ir recv"])
    text = out.read_text()
    assert '#include "core/NullWriter.h"' in text
    assert 'extern "C" void commander_run_autostart(CommandRegistry &reg)' in text
    assert 'NullWriter _null;' in text
    assert 'reg.dispatch("ir recv", _null);' in text


def test_codegen_no_autostart_omits_function(cli_mod, tmp_path):
    out = tmp_path / "commander_modules.h"
    cli_mod.generate_modules_file("unoq", {}, out)            # no autostart
    text = out.read_text()
    assert "commander_run_autostart" not in text
    assert "NullWriter" not in text


def test_codegen_escapes_quotes(cli_mod, tmp_path):
    out = tmp_path / "commander_modules.h"
    cli_mod.generate_modules_file("pico", {}, out, autostart=['say "hi"'])
    text = out.read_text()
    assert r'reg.dispatch("say \"hi\"", _null);' in text


def test_multiple_commands_in_order(cli_mod, tmp_path):
    out = tmp_path / "commander_modules.h"
    cli_mod.generate_modules_file("unoq", {}, out, autostart=["ir recv", "aicam stream on"])
    text = out.read_text()
    i1 = text.index('reg.dispatch("ir recv"')
    i2 = text.index('reg.dispatch("aicam stream on"')
    assert i1 < i2, "autostart commands must dispatch in order"


def test_manifest_roundtrips_autostart(cli_mod, tmp_path):
    p = tmp_path / "cmdr.toml"
    cli_mod.write_manifest(p, "unoq", {"ir": {"gpio": 5}}, ["ir recv"])
    target, modules, autostart = cli_mod.read_manifest(p)
    assert target == "unoq"
    assert modules == {"ir": {"gpio": 5}}
    assert autostart == ["ir recv"]
    assert "[autostart]" in p.read_text()


# ── CLI: cmdr autostart add/list/clear, and survival across a module enable ──────

def init_args(target, name="proj"):
    return Namespace(target=target, name=name, chip="esp32s3", flash=16, psram=8)


@pytest.fixture
def answer_defaults(monkeypatch):
    monkeypatch.setattr("builtins.input", lambda *_a, **_k: "")


def _enter(cli_mod, project_dir, target, name="proj"):
    import os
    cli_mod.cmd_init(init_args(target, name))
    os.chdir(project_dir.path / name)


def test_cmd_autostart_add_then_clear(cli_mod, project_dir):
    _enter(cli_mod, project_dir, "unoq")
    root = project_dir.path / "proj"

    cli_mod.cmd_autostart(Namespace(command="autostart", action="add", cmdline="ir recv"))
    _t, _m, autostart = cli_mod.read_manifest(root / "cmdr.toml")
    assert autostart == ["ir recv"]
    # regenerated file carries the dispatch
    assert "commander_run_autostart" in (root / "src" / "commander_modules.h").read_text()

    cli_mod.cmd_autostart(Namespace(command="autostart", action="clear"))
    _t, _m, autostart = cli_mod.read_manifest(root / "cmdr.toml")
    assert autostart == []
    assert "commander_run_autostart" not in (root / "src" / "commander_modules.h").read_text()


def test_autostart_survives_module_enable(cli_mod, project_dir, answer_defaults):
    """Enabling a module must not wipe an existing [autostart] (the round-trip bug guard)."""
    _enter(cli_mod, project_dir, "unoq")
    root = project_dir.path / "proj"

    cli_mod.cmd_autostart(Namespace(command="autostart", action="add", cmdline="ir recv"))
    cli_mod.cmd_module(Namespace(command="module", action="enable", name="ir"))

    _t, modules, autostart = cli_mod.read_manifest(root / "cmdr.toml")
    assert "ir" in modules, "module enabled"
    assert autostart == ["ir recv"], "autostart preserved across enable"
    # and the regenerated file still dispatches it
    assert 'reg.dispatch("ir recv", _null);' in (root / "src" / "commander_modules.h").read_text()
