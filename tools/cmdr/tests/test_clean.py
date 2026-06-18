"""`cmdr clean` — remove build artifacts for a fresh build, without touching source.

Wipes CMake build dirs (build/, build-<target>/, incl. their FetchContent _deps),
PlatformIO's .pio/, and esp32's generated sdkconfig; leaves source + cmdr-generated
project files. Guarded so it refuses outside a commander project (it does rmtree).
"""
import pytest


def _layout(root):
    """A throwaway project with build artifacts + source to protect."""
    (root / "cmdr.toml").write_text('target = "esp32"\n')
    (root / "main.cpp").write_text("int main(){}\n")
    (root / "sdkconfig.defaults").write_text("CONFIG_X=y\n")
    (root / "sdkconfig").write_text("GENERATED\n")
    for bd in ("build-esp32", "build-unoq", "build-pico", "build"):
        d = root / bd
        (d / "_deps" / "commander-src").mkdir(parents=True)
        (d / "CMakeCache.txt").write_text("x\n")
    (root / ".pio" / "libdeps").mkdir(parents=True)
    (root / ".pio" / "x").write_text("x\n")


def test_clean_removes_artifacts_keeps_source(cli_mod, project_dir):
    root = project_dir.path
    _layout(root)
    cli_mod.cmd_clean()

    # build dirs + .pio + generated sdkconfig gone
    for gone in ("build-esp32", "build-unoq", "build-pico", "build", ".pio", "sdkconfig"):
        assert not (root / gone).exists(), f"{gone} should have been cleaned"
    # source + cmdr-generated config preserved
    for keep in ("cmdr.toml", "main.cpp", "sdkconfig.defaults"):
        assert (root / keep).exists(), f"{keep} must be preserved"


def test_clean_refuses_outside_project(cli_mod, project_dir):
    """rmtree is destructive — refuse if there's no project marker, and leave dirs intact."""
    root = project_dir.path
    (root / "build-esp32").mkdir()           # looks like a build dir, but no cmdr.toml etc.
    with pytest.raises(SystemExit):
        cli_mod.cmd_clean()
    assert (root / "build-esp32").exists(), "must not delete anything when refusing"


def test_clean_noop_when_nothing_to_clean(cli_mod, project_dir, capsys):
    root = project_dir.path
    (root / "cmdr.toml").write_text('target = "pico"\n')
    cli_mod.cmd_clean()                       # no build dirs present
    assert "nothing to clean" in capsys.readouterr().out


def test_clean_keeps_pio_project_source(cli_mod, project_dir):
    """A PlatformIO project: .pio/ goes, platformio.ini + src stay."""
    root = project_dir.path
    (root / "platformio.ini").write_text("[env:x]\n")
    (root / ".pio" / "build").mkdir(parents=True)
    cli_mod.cmd_clean()
    assert not (root / ".pio").exists()
    assert (root / "platformio.ini").exists()
