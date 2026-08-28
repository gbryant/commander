"""`cmdr module enable/disable` end-to-end on a scaffolded project — idempotence,
tool install/remove, MAX_COMMANDS sync, and the honest-menu refusal. Drives the real
cmd_module() with input()/subprocess stubbed (project_dir fixture), so every codegen,
manifest, and bin/ side effect is the production path.
"""
from argparse import Namespace

import pytest


def init_args(target, name="proj"):
    return Namespace(target=target, name=name, chip="esp32s3", flash=16, psram=8)


def mod_args(action, name=None):
    return Namespace(command="module", action=action, name=name)


@pytest.fixture
def answer_defaults(monkeypatch, cli_mod):
    """Answer every enable question with its default (empty input)."""
    monkeypatch.setattr("builtins.input", lambda *_a, **_k: "")


def _enter(cli_mod, project_dir, target, name="proj"):
    cli_mod.cmd_init(init_args(target, name))
    import os
    os.chdir(project_dir.path / name)


def test_enable_is_idempotent(cli_mod, project_dir, answer_defaults):
    """Enabling the same module twice leaves an identical manifest + generated file."""
    _enter(cli_mod, project_dir, "pico")
    cli_mod.cmd_module(mod_args("enable", "ir"))
    toml1 = (project_dir.path / "proj" / "cmdr.toml").read_text()
    gen1 = (project_dir.path / "proj" / "commander_modules.h").read_text()

    cli_mod.cmd_module(mod_args("enable", "ir"))
    toml2 = (project_dir.path / "proj" / "cmdr.toml").read_text()
    gen2 = (project_dir.path / "proj" / "commander_modules.h").read_text()

    assert toml1 == toml2, "re-enabling changed the manifest"
    assert gen1 == gen2, "re-enabling changed the generated file"


def test_enable_then_disable_returns_to_base(cli_mod, project_dir, answer_defaults):
    _enter(cli_mod, project_dir, "pico")
    base = (project_dir.path / "proj" / "commander_modules.h").read_text()

    cli_mod.cmd_module(mod_args("enable", "controller"))
    after_enable = (project_dir.path / "proj" / "commander_modules.h").read_text()
    assert after_enable != base
    _t, mods, _as = cli_mod.read_manifest(project_dir.path / "proj" / "cmdr.toml")
    assert "controller" in mods

    cli_mod.cmd_module(mod_args("disable", "controller"))
    after_disable = (project_dir.path / "proj" / "commander_modules.h").read_text()
    assert after_disable == base, "disable should restore the base generated file"
    _t, mods, _as = cli_mod.read_manifest(project_dir.path / "proj" / "cmdr.toml")
    assert "controller" not in mods


def test_ir_installs_and_removes_tools(cli_mod, project_dir, answer_defaults):
    """Enabling ir drops its host tools + seeds maps/; disabling removes the tools but
    keeps the (possibly user-edited) maps/ dir."""
    _enter(cli_mod, project_dir, "pico")
    root = project_dir.path / "proj"

    cli_mod.cmd_module(mod_args("enable", "ir"))
    assert (root / "bin" / "irmap.py").exists()
    assert (root / "bin" / "irlookup.py").exists()
    assert (root / "bin" / "find_port.py").exists()
    assert (root / "maps").is_dir() and any((root / "maps").glob("*.json")), "maps/ seeded"

    cli_mod.cmd_module(mod_args("disable", "ir"))
    assert not (root / "bin" / "irmap.py").exists(), "tool removed on disable"
    assert (root / "maps").is_dir(), "maps/ (user data) preserved on disable"


def test_max_commands_grows_but_never_shrinks(cli_mod, project_dir, answer_defaults):
    """MAX_COMMANDS grows to fit new modules, and is never lowered again.

    The computed value covers cmdr-managed modules plus a small reserve — it has no
    idea how many commands the app registers itself, and an app can easily have more
    than the modules do (cmdr-ipstube has 15). Shrinking on `disable` would size the
    registry array below what the firmware actually needs and start dropping
    commands, so a larger existing value is left alone."""
    _enter(cli_mod, project_dir, "pico")
    import re
    cmake = project_dir.path / "proj" / "CMakeLists.txt"

    def maxcmds():
        m = re.search(r"add_compile_definitions\(MAX_COMMANDS=(\d+)\)", cmake.read_text())
        return int(m.group(1)) if m else None

    base = maxcmds()
    assert base is not None
    cli_mod.cmd_module(mod_args("enable", "controller"))
    grown = maxcmds()
    assert grown > base, "MAX_COMMANDS should grow when a module is added"
    cli_mod.cmd_module(mod_args("disable", "controller"))
    assert maxcmds() == grown, (
        "MAX_COMMANDS must not shrink on disable — the computed value ignores "
        "app-registered commands, so lowering it can drop them")


def test_controller_injects_and_reverts_cmake(cli_mod, project_dir, answer_defaults):
    """The controller module toggles the Bluetooth CMake injection (enable→present,
    disable→gone)."""
    _enter(cli_mod, project_dir, "pico")
    cmake = project_dir.path / "proj" / "CMakeLists.txt"

    cli_mod.cmd_module(mod_args("enable", "controller"))
    assert "CYW43_ENABLE_BLUETOOTH" in cmake.read_text()
    assert "COMMANDER_ENABLE_CONTROLLER" in cmake.read_text()

    cli_mod.cmd_module(mod_args("disable", "controller"))
    assert "CYW43_ENABLE_BLUETOOTH" not in cmake.read_text()
    assert "COMMANDER_ENABLE_CONTROLLER" not in cmake.read_text()


def test_unsupported_module_refused(cli_mod, project_dir, answer_defaults):
    """The honest menu: enabling an esp32-only module on pico dies, no file churn."""
    _enter(cli_mod, project_dir, "pico")
    gen_before = (project_dir.path / "proj" / "commander_modules.h").read_text()
    with pytest.raises(SystemExit):
        cli_mod.cmd_module(mod_args("enable", "ipstube"))
    assert (project_dir.path / "proj" / "commander_modules.h").read_text() == gen_before


def test_serial1_owners_mutually_exclusive(cli_mod, project_dir, answer_defaults):
    """roomba and loco-bridge both own Serial1 — enabling one blocks the other."""
    _enter(cli_mod, project_dir, "r4")
    cli_mod.cmd_module(mod_args("enable", "roomba"))
    with pytest.raises(SystemExit):
        cli_mod.cmd_module(mod_args("enable", "loco-bridge"))


def test_panels_mutually_exclusive(cli_mod, project_dir, answer_defaults):
    """st7796 and st7789 both register `lcd` — the second would be shadowed at
    dispatch rather than rejected, so cmdr refuses the combination outright."""
    _enter(cli_mod, project_dir, "pico2")
    cli_mod.cmd_module(mod_args("enable", "st7796"))
    with pytest.raises(SystemExit):
        cli_mod.cmd_module(mod_args("enable", "st7789"))
    # ...and the other way round, once the first is out of the way.
    cli_mod.cmd_module(mod_args("disable", "st7796"))
    cli_mod.cmd_module(mod_args("enable", "st7789"))
    with pytest.raises(SystemExit):
        cli_mod.cmd_module(mod_args("enable", "st7796"))


def test_st7789_panel_preset_sets_ram_geometry(cli_mod, project_dir, answer_defaults):
    """The panel preset is what makes the window offset correct — a 240x135 glass
    on 240x320 of controller RAM. Wrong RAM values are invisible until hardware."""
    _enter(cli_mod, project_dir, "pico2")
    cli_mod.cmd_module(mod_args("enable", "st7789"))
    gen = (project_dir.path / "proj" / "commander_modules.h").read_text()
    assert "c.nativeW = 135; c.nativeH = 240;" in gen
    assert "c.ramW = 240; c.ramH = 320;" in gen


def test_libraries_only_gates_runner_modules(cli_mod, project_dir, answer_defaults):
    """A project that supplies its own runner can't have modules whose targets or
    hooks live in runners/ — they'd compile and fail to link. The honest menu
    hides them and enable refuses."""
    _enter(cli_mod, project_dir, "pico2")
    toml = project_dir.path / "proj" / "cmdr.toml"
    toml.write_text(toml.read_text().replace('target = "pico2"',
                                             'target = "pico2"\nlibraries_only = true'))
    assert cli_mod.libraries_only(toml)
    assert not cli_mod._module_supported("wifi", "pico2", True)
    assert not cli_mod._module_supported("ir", "pico2", True)
    # ...but the portable ones are unaffected.
    assert cli_mod._module_supported("st7789", "pico2", True)
    assert cli_mod._module_supported("buttons", "pico2", True)
    with pytest.raises(SystemExit):
        cli_mod.cmd_module(mod_args("enable", "wifi"))


def test_libraries_only_survives_a_module_change(cli_mod, project_dir, answer_defaults):
    """Enabling a module rewrites cmdr.toml — the flag must not be dropped, or the
    project silently becomes a normal runner-based one."""
    _enter(cli_mod, project_dir, "pico2")
    toml = project_dir.path / "proj" / "cmdr.toml"
    toml.write_text(toml.read_text().replace('target = "pico2"',
                                             'target = "pico2"\nlibraries_only = true'))
    cli_mod.cmd_module(mod_args("enable", "st7789"))
    assert cli_mod.libraries_only(toml), "libraries_only was dropped by module enable"
    assert "st7789" in toml.read_text()


def test_ir_pio_lib_dep_added_on_r4(cli_mod, project_dir, answer_defaults):
    """On a PlatformIO target, enabling ir adds IRremote to lib_deps; disable removes it."""
    _enter(cli_mod, project_dir, "r4")
    pio = project_dir.path / "proj" / "platformio.ini"
    cli_mod.cmd_module(mod_args("enable", "ir"))
    assert "IRremote" in pio.read_text()
    cli_mod.cmd_module(mod_args("disable", "ir"))
    assert "IRremote" not in pio.read_text()
