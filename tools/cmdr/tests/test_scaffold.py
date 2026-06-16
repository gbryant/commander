"""`cmdr init <board>` scaffolding — each target produces its expected file set, a
valid cmdr.toml, and a generated commander_modules.h in the board's layout. Runs
with subprocess stubbed (project_dir fixture), so no cmake/idf/pio is invoked.
"""
from argparse import Namespace

import pytest


def init_args(target, name="proj", **kw):
    return Namespace(target=target, name=name, chip=kw.get("chip", "esp32s3"),
                     flash=kw.get("flash", 16), psram=kw.get("psram", 8))


# (target, [files relative to project dir that must exist], modules.h relative path)
EXPECTED = {
    "pico":     (["CMakeLists.txt", "main.cpp", "secrets.h", "cmdr.toml",
                  "FreeRTOS_Kernel_import.cmake"], "commander_modules.h"),
    "pico2":    (["CMakeLists.txt", "main.cpp", "secrets.h", "cmdr.toml"], "commander_modules.h"),
    "uno":      (["platformio.ini", "src/main.cpp", "cmdr.toml", "scripts/find_port.py",
                  "bum", "build", "upload", "monitor"], "src/commander_modules.h"),
    "r4":       (["platformio.ini", "secrets.h", "src/main.cpp", "cmdr.toml",
                  "scripts/find_port.py", "bum", "build", "upload", "monitor", "bum-ota"],
                 "src/commander_modules.h"),
    "bluepill": (["platformio.ini", "src/main.cpp", "cmdr.toml", "scripts/stm32_build.py",
                  "bum", "build", "upload", "monitor"], "src/commander_modules.h"),
    "unoq":     (["CMakeLists.txt", "prj.conf", "app.overlay", "README.md", "src/main.cpp",
                  "cmdr.toml", "build", "flash", "monitor", "bum", "enable-flash-boot",
                  "install-broker", "restore-arduino", "deploy-sbc"], "src/commander_modules.h"),
    "esp32":    (["CMakeLists.txt", "sdkconfig.defaults", "secrets.h", "main/CMakeLists.txt",
                  "main/main.cpp", "cmdr.toml", "scripts/find_port.py",
                  "bum", "build", "upload", "monitor"], "main/commander_modules.h"),
}


@pytest.mark.parametrize("target", list(EXPECTED))
def test_init_file_set(cli_mod, project_dir, target):
    files, modules_h = EXPECTED[target]
    cli_mod.cmd_init(init_args(target))
    root = project_dir.path / "proj"

    for rel in files:
        assert (root / rel).exists(), f"{target}: missing {rel}"

    # generated registration file is present in the board's layout and well-formed
    mh = root / modules_h
    assert mh.exists(), f"{target}: missing {modules_h}"
    text = mh.read_text()
    assert "commander_register_modules" in text and "_m_system" in text

    # manifest records the right target and starts with no optional modules
    t, mods = cli_mod.read_manifest(root / "cmdr.toml")
    assert t == target
    assert mods == {}


def test_init_rejects_existing_dir(cli_mod, project_dir):
    cli_mod.cmd_init(init_args("pico", name="dup"))
    with pytest.raises(SystemExit):
        cli_mod.cmd_init(init_args("pico", name="dup"))


def test_init_rejects_bad_name(cli_mod, project_dir):
    with pytest.raises(SystemExit):
        cli_mod.cmd_init(init_args("pico", name="bad/name"))


def test_pico_init_configures_cmake(cli_mod, project_dir):
    """Pico scaffold kicks off a cmake configure (the only target that does)."""
    cli_mod.cmd_init(init_args("pico"))
    assert any(c[0] == "cmake" for c in project_dir.calls), "pico init should run cmake -B"


def test_esp32_layout_has_main_component(cli_mod, project_dir):
    cli_mod.cmd_init(init_args("esp32"))
    root = project_dir.path / "proj"
    # generated file + main.cpp both live in the IDF component dir main/
    assert (root / "main" / "commander_modules.h").exists()
    assert (root / "main" / "main.cpp").exists()
    # no cmake/idf invoked at scaffold time for esp32 (first build does set-target)
    assert not any(c and c[0] in ("cmake", "idf.py") for c in project_dir.calls)


def test_esp32_build_scripts_self_source_idf(cli_mod, project_dir, monkeypatch):
    """build/upload self-source ESP-IDF so the user needn't run `esp` first: the env
    preamble is present, honors $IDF_EXPORT, and bakes the init-time $IDF_PATH default."""
    monkeypatch.setenv("IDF_PATH", "/opt/esp/esp-idf")
    cli_mod.cmd_init(init_args("esp32"))
    root = project_dir.path / "proj"

    for script in ("build", "upload"):
        text = (root / script).read_text()
        assert "command -v idf.py" in text, f"{script} should guard on idf.py presence"
        assert "${IDF_EXPORT:-}" in text, f"{script} should honor $IDF_EXPORT override"
        assert "/opt/esp/esp-idf/export.sh" in text, f"{script} should bake the init-time IDF_PATH"

    # monitor uses tio only — it should NOT carry the IDF preamble
    assert "command -v idf.py" not in (root / "monitor").read_text()


def test_esp32_build_scripts_blank_default_without_idf(cli_mod, project_dir, monkeypatch):
    """If IDF wasn't active at init, no path is baked (the slot is blank, falling back to
    $IDF_EXPORT / standard locations at runtime) — not a broken literal."""
    monkeypatch.delenv("IDF_PATH", raising=False)
    cli_mod.cmd_init(init_args("esp32"))
    text = (project_dir.path / "proj" / "build").read_text()
    assert "__IDF_EXPORT__" not in text, "the placeholder must be rendered out"
    assert '"" "${IDF_PATH:-}/export.sh"' in text, "blank baked default, runtime fallbacks intact"
