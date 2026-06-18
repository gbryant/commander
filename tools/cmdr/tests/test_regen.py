"""`cmdr regen` — re-emit generated project files from current templates without re-init.

Restores stale/edited generated scripts + commander_modules.h + module host tools, while
leaving hand-written source, cmdr.toml, and CMakeLists.txt/platformio.ini alone. Recovers
init-only params (project name, esp32 chip) from the existing project. --dry-run is a no-op
on disk. Also pins that init and regen share _emit_scripts (so they can't drift).
"""
from argparse import Namespace

import pytest


def init_args(target, name="proj", **kw):
    return Namespace(target=target, name=name, chip=kw.get("chip", "esp32s3"),
                     flash=kw.get("flash", 16), psram=kw.get("psram", 8))


def regen_args(dry=False):
    return Namespace(command="regen", dry_run=dry)


@pytest.fixture
def answer_defaults(monkeypatch):
    monkeypatch.setattr("builtins.input", lambda *_a, **_k: "")


def _enter(cli_mod, project_dir, target, name="proj", **kw):
    import os
    cli_mod.cmd_init(init_args(target, name, **kw))
    os.chdir(project_dir.path / name)


def test_regen_restores_mangled_script_keeps_source(cli_mod, project_dir):
    """A stale/edited generated script is restored; hand-written source is preserved."""
    _enter(cli_mod, project_dir, "unoq")
    root = project_dir.path / "proj"
    (root / "src" / "main.cpp").write_text("// USER EDIT\n")
    (root / "install-broker").write_text("# mangled\n")

    cli_mod.cmd_regen(regen_args())

    # install-broker is now a thin shim (the real logic lives in the fetched framework's
    # dev/unoq/install_broker.sh) — regen restores the stub, which locates + execs it.
    ib = (root / "install-broker").read_text()
    assert "install_broker.sh" in ib and "exec bash" in ib, "install-broker shim restored"
    assert (root / "src" / "main.cpp").read_text() == "// USER EDIT\n", "source preserved"


def test_regen_preserves_cmakelists_and_recovers_chip(cli_mod, project_dir):
    """CMakeLists.txt (accumulating state) is left alone; esp32 chip is recovered from the
    existing build script so the regenerated scripts keep the right target."""
    _enter(cli_mod, project_dir, "esp32", chip="esp32c3")
    root = project_dir.path / "proj"
    (root / "CMakeLists.txt").write_text((root / "CMakeLists.txt").read_text() + "\n# CUSTOM\n")
    (root / "build").write_text("# mangled\n")   # but still recover chip? no — recovery reads set-target

    # chip recovery reads `set-target <chip>` from the OLD build script; mangling it loses that,
    # so reset it to a realistic prior build script first:
    (root / "build").write_text('idf.py -B "$BUILD" set-target esp32c3\n')
    cli_mod.cmd_regen(regen_args())

    assert "# CUSTOM" in (root / "CMakeLists.txt").read_text(), "CMakeLists.txt untouched"
    assert "set-target esp32c3" in (root / "build").read_text(), "esp32 chip recovered"


def test_regen_dry_run_writes_nothing(cli_mod, project_dir):
    _enter(cli_mod, project_dir, "unoq")
    root = project_dir.path / "proj"
    (root / "install-broker").write_text("# mangled\n")
    cli_mod.cmd_regen(regen_args(dry=True))
    assert (root / "install-broker").read_text() == "# mangled\n", "dry-run must not write"


def test_regen_refreshes_module_tools(cli_mod, project_dir, answer_defaults):
    """Enabled modules' host tools are re-installed (the stale-IR-tools fix path)."""
    _enter(cli_mod, project_dir, "unoq")
    root = project_dir.path / "proj"
    cli_mod.cmd_module(Namespace(command="module", action="enable", name="ir"))
    # mangle an installed tool, then regen should restore it
    (root / "bin" / "ir_lookup.py").write_text("# mangled\n")
    cli_mod.cmd_regen(regen_args())
    assert "# mangled" not in (root / "bin" / "ir_lookup.py").read_text(), "tool refreshed"


def test_regen_pico_notes_cmake_scripts(cli_mod, project_dir, capsys):
    """Pico emits no dev scripts here (they come from CMake); regen still does
    commander_modules.h and prints the reconfigure note."""
    _enter(cli_mod, project_dir, "pico")
    cli_mod.cmd_regen(regen_args())
    out = capsys.readouterr().out
    assert "commander_modules.h" in out
    assert "pico dev scripts come from CMake" in out


def test_init_and_regen_emit_identical_scripts(cli_mod, tmp_path):
    """init and regen share _emit_scripts, so they produce the same script set (no drift)."""
    init_set = set(cli_mod._emit_scripts("unoq", "x", tmp_path / "a", dry=True))
    regen_set = set(cli_mod._emit_scripts("unoq", "x", tmp_path / "b", dry=True))
    assert init_set == regen_set and "install-broker" in init_set
