"""`cmdr enable debug` — the SWD wiring generator (Tier 1, see docs/testing.md).

Two properties matter more than the file contents and get most of the attention
here:

  * **[debug] survives a debug-unaware `write_manifest`.** Every `cmdr module
    enable/disable` rewrites cmdr.toml through callers that know nothing about
    this feature. If the section were dropped, ./flash and ./debug would be left
    orphaned with nothing to regenerate them from — the exact trap
    `_extra_top_keys` already exists for.
  * **`cmdr regen` puts the files back.** They are gitignored, so regen is the
    only route back after a fresh clone. A regen that silently skipped them
    would leave a cloned project unable to flash.

The honest-menu property is tested too: a target with no working openocd config
must refuse rather than emit a script that cannot work.
"""
import os
from pathlib import Path

import pytest


@pytest.fixture
def project(tmp_path, monkeypatch):
    """A minimal pico2 project root, cwd'd into."""
    (tmp_path / "cmdr.toml").write_text(
        'target = "pico2"\n\n[module.leds]\npins = "16,17"\n')
    (tmp_path / "CMakeLists.txt").write_text("project(demo)\n")
    monkeypatch.chdir(tmp_path)
    return tmp_path


# ── the manifest record ──────────────────────────────────────────────────────

def test_enable_records_debug_section(cli_mod, project):
    cli_mod.enable_debug()
    assert cli_mod._read_debug(project / "cmdr.toml") == {
        "probe": "cmsis-dap", "target_cfg": "target/rp2350.cfg", "speed": 5000,
        "build_dir": "build-pico2"}


def test_debug_survives_a_debug_unaware_write_manifest(cli_mod, project):
    """The regression this feature is most likely to grow: `cmdr module enable`
    rewrites the manifest without ever hearing about [debug]."""
    cli_mod.enable_debug()
    m = project / "cmdr.toml"
    target, modules, autostart = cli_mod.read_manifest(m)
    modules["buzzer"] = {"pin": 13}
    cli_mod.write_manifest(m, target, modules, autostart)   # no debug= argument

    assert cli_mod._read_debug(m)["probe"] == "cmsis-dap"
    assert "buzzer" in cli_mod.read_manifest(m)[1]


def test_explicit_empty_debug_clears_it(cli_mod, project):
    cli_mod.enable_debug()
    m = project / "cmdr.toml"
    target, modules, autostart = cli_mod.read_manifest(m)
    cli_mod.write_manifest(m, target, modules, autostart, debug={})
    assert cli_mod._read_debug(m) == {}


def test_read_debug_ignores_other_sections(cli_mod, tmp_path):
    p = tmp_path / "cmdr.toml"
    p.write_text('target = "pico2"\n\n[module.buzzer]\npin = 13\n\n[autostart]\ncmd0 = "led 0 on"\n')
    assert cli_mod._read_debug(p) == {}


# ── the emitted files ────────────────────────────────────────────────────────

def test_enable_writes_all_four_files_executable(cli_mod, project):
    cli_mod.enable_debug()
    for s in cli_mod.DEBUG_SCRIPTS:
        f = project / s
        assert f.exists(), f"{s} not written"
        assert os.access(f, os.X_OK), f"{s} not executable"
    assert (project / "openocd.cfg").exists()


def test_openocd_cfg_carries_probe_target_and_rp2350_quirk(cli_mod, project):
    cli_mod.enable_debug()
    cfg = (project / "openocd.cfg").read_text()
    assert "source [find interface/cmsis-dap.cfg]" in cfg
    assert "source [find target/rp2350.cfg]" in cfg
    assert "transport select swd" in cfg
    # The gotcha paid for on real hardware: without this a reset with both cores
    # running leaves core1 somewhere gdb can't recover from.
    assert "rp2350.dap.core1 cortex_m reset_config sysresetreq" in cfg
    # The escape hatch for anything the generator doesn't model.
    assert "openocd-local.cfg" in cfg


def test_rp2040_gets_no_core1_quirk(cli_mod, project):
    (project / "cmdr.toml").write_text('target = "pico"\n')
    cli_mod.enable_debug()
    cfg = (project / "openocd.cfg").read_text()
    assert "source [find target/rp2040.cfg]" in cfg
    assert "sysresetreq" not in cfg          # single core: nothing to work around


def test_probe_override(cli_mod, project):
    cli_mod.enable_debug(probe="jlink")
    assert "interface/jlink.cfg" in (project / "openocd.cfg").read_text()
    assert cli_mod._read_debug(project / "cmdr.toml")["probe"] == "jlink"


def test_shims_pass_the_right_action_and_build_dir(cli_mod, project):
    cli_mod.enable_debug()
    for s in cli_mod.DEBUG_SCRIPTS:
        body = (project / s).read_text()
        assert f'"$IMPL" {s} --cfg openocd.cfg' in body, f"{s} invokes the wrong action"
        assert '--build-dir "build-pico2"' in body


def test_bluepill_uses_stlink_and_the_pio_build_dir(cli_mod, project):
    (project / "cmdr.toml").write_text('target = "bluepill"\n')
    (project / "platformio.ini").write_text("[env:bluepill_f103c8]\nplatform = ststm32\n")
    cli_mod.enable_debug()
    cfg = (project / "openocd.cfg").read_text()
    assert "interface/stlink.cfg" in cfg
    assert "target/stm32f1x.cfg" in cfg
    assert '--build-dir ".pio/build/bluepill_f103c8"' in (project / "flash").read_text()


# ── the honest menu ──────────────────────────────────────────────────────────

@pytest.mark.parametrize("target", ["esp32", "uno", "r4", "unoq"])
def test_targets_without_a_working_swd_story_are_refused(cli_mod, project, target):
    """Emitting a ./flash for a target that cannot use one is the same dishonesty
    the module menu already avoids. r4 in particular: openocd 0.12 ships no
    renesas_ra config, so the script would exist and always fail."""
    (project / "cmdr.toml").write_text(f'target = "{target}"\n')
    with pytest.raises(SystemExit):
        cli_mod.enable_debug()
    assert not (project / "flash").exists()
    assert not (project / "openocd.cfg").exists()


# ── regen: the only way back after a clone ───────────────────────────────────

def test_regen_reproduces_the_files(cli_mod, project):
    cli_mod.enable_debug()
    for f in cli_mod.DEBUG_FILES:
        (project / f).unlink()                       # a fresh clone: gitignored, so absent

    written = cli_mod._emit_scripts("pico2", "demo", project)
    assert set(cli_mod.DEBUG_FILES) <= set(written)
    for f in cli_mod.DEBUG_FILES:
        assert (project / f).exists(), f"regen did not restore {f}"


def test_regen_dry_run_lists_without_writing(cli_mod, project):
    cli_mod.enable_debug()
    for f in cli_mod.DEBUG_FILES:
        (project / f).unlink()
    written = cli_mod._emit_scripts("pico2", "demo", project, dry=True)
    assert set(cli_mod.DEBUG_FILES) <= set(written)
    assert not (project / "flash").exists()


def test_regen_emits_nothing_when_debug_is_off(cli_mod, project):
    written = cli_mod._emit_scripts("pico2", "demo", project)
    assert not set(cli_mod.DEBUG_FILES) & set(written)


# ── gitignore + disable ──────────────────────────────────────────────────────

def test_gitignore_gains_one_clean_block(cli_mod, project):
    (project / ".gitignore").write_text("secrets.h\nbuild/\n")
    cli_mod.enable_debug()
    lines = (project / ".gitignore").read_text().splitlines()
    assert sum(1 for line in lines if line.startswith("# cmdr-generated SWD")) == 1
    # One blank line separates the block from what was already there, and the
    # block itself is contiguous — no stray gaps between the paths.
    start = next(i for i, line in enumerate(lines) if line.startswith("# cmdr-generated SWD"))
    block = lines[start + 1:]
    assert block == [f"/{f}" for f in cli_mod.DEBUG_FILES]
    assert lines[start - 1] == ""
    assert lines[:start - 1] == ["secrets.h", "build/"]


def test_gitignore_is_idempotent(cli_mod, project):
    cli_mod.enable_debug()
    first = (project / ".gitignore").read_text()
    cli_mod.enable_debug()
    assert (project / ".gitignore").read_text() == first


def test_disable_removes_files_and_section(cli_mod, project):
    cli_mod.enable_debug()
    cli_mod.disable_debug()
    assert cli_mod._read_debug(project / "cmdr.toml") == {}
    for f in cli_mod.DEBUG_FILES:
        assert not (project / f).exists()
    # modules are untouched by the round trip
    assert "leds" in cli_mod.read_manifest(project / "cmdr.toml")[1]


def test_disable_keeps_the_hand_written_local_config(cli_mod, project):
    cli_mod.enable_debug()
    (project / "openocd-local.cfg").write_text("# mine\n")
    cli_mod.disable_debug()
    assert (project / "openocd-local.cfg").exists()


def test_debug_min_tag_is_not_ahead_of_what_scaffolds_pin(cli_mod):
    """A fresh project must never be pinned below what `cmdr enable debug` needs.
    Sibling of test_scaffold's FRAMEWORK_TAG >= MIN_FRAMEWORK_TAG guard: this one
    catches forgetting to bump FRAMEWORK_TAG when cutting the release that
    carries scripts/swd.sh."""
    have = cli_mod._parse_release(cli_mod.FRAMEWORK_TAG)
    need = cli_mod._parse_release(cli_mod.DEBUG_MIN_TAG)
    assert have and need, "both must be vMAJOR.MINOR release tags"
    assert have >= need, (
        f"scaffolds pin {cli_mod.FRAMEWORK_TAG} but `cmdr enable debug` needs "
        f"{cli_mod.DEBUG_MIN_TAG} - a fresh project could not enable it")


# ── libraries-only projects ──────────────────────────────────────────────────
#
# These own their build (their own main/FreeRTOS/USB and their own dev scripts),
# so cmdr cannot derive where the ELF lands — cmdr-probe builds into build-geek,
# not build-pico2. They still get SWD files, because those are not part of the
# build: they need only the ELF's directory, and [debug].build_dir names it.

@pytest.fixture
def libs_only_project(tmp_path, monkeypatch):
    (tmp_path / "cmdr.toml").write_text('target = "pico2"\nlibraries_only = true\n')
    (tmp_path / "CMakeLists.txt").write_text("project(probe)\n")
    monkeypatch.chdir(tmp_path)
    return tmp_path


def test_libraries_only_requires_an_explicit_build_dir(cli_mod, libs_only_project):
    """Guessing build-pico2 would emit a script that looks right and never finds
    the firmware, so it asks instead."""
    with pytest.raises(SystemExit):
        cli_mod.enable_debug()
    assert not (libs_only_project / "flash").exists()
    assert cli_mod._read_debug(libs_only_project / "cmdr.toml") == {}


def test_libraries_only_with_a_build_dir_works(cli_mod, libs_only_project):
    cli_mod.enable_debug(build_dir="build-geek")
    assert cli_mod._read_debug(libs_only_project / "cmdr.toml")["build_dir"] == "build-geek"
    assert '--build-dir "build-geek"' in (libs_only_project / "flash").read_text()


def test_regen_restores_a_libraries_only_projects_swd_files(cli_mod, libs_only_project):
    """The v1.3 bug this fixes: enable wrote and gitignored the files, then regen
    never put them back, so a fresh clone silently lost its SWD tooling."""
    cli_mod.enable_debug(build_dir="build-geek")
    for f in cli_mod.DEBUG_FILES:
        (libs_only_project / f).unlink()

    written = cli_mod._emit_scripts("pico2", "probe", libs_only_project)
    assert set(cli_mod.DEBUG_FILES) <= set(written), \
        "regen skipped the SWD files on a libraries-only project"
    for f in cli_mod.DEBUG_FILES:
        assert (libs_only_project / f).exists()
    assert '--build-dir "build-geek"' in (libs_only_project / "flash").read_text(), \
        "regen re-derived the build dir instead of reading [debug]"


def test_regen_still_skips_dev_scripts_for_libraries_only(cli_mod, libs_only_project):
    """The gate still does its original job: cmdr must not overwrite the build
    and dev scripts a libraries-only project owns."""
    cli_mod.enable_debug(build_dir="build-geek")
    written = cli_mod._emit_scripts("uno", "probe", libs_only_project)
    assert "bum" not in written and "build" not in written


def test_explicit_build_dir_wins_on_a_normal_project(cli_mod, project):
    cli_mod.enable_debug(build_dir="out/firmware/")
    assert cli_mod._read_debug(project / "cmdr.toml")["build_dir"] == "out/firmware"
