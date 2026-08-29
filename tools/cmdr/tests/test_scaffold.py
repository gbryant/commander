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
    "pico":     ([".gitignore", "CMakeLists.txt", "main.cpp", "secrets.h", "cmdr.toml",
                  "FreeRTOS_Kernel_import.cmake"], "commander_modules.h"),
    "pico2":    ([".gitignore", "CMakeLists.txt", "main.cpp", "secrets.h", "cmdr.toml"], "commander_modules.h"),
    "uno":      ([".gitignore", "platformio.ini", "src/main.cpp", "cmdr.toml", "scripts/find_port.py",
                  "bum", "build", "upload", "monitor"], "src/commander_modules.h"),
    "r4":       ([".gitignore", "platformio.ini", "secrets.h", "src/main.cpp", "cmdr.toml",
                  "scripts/find_port.py", "bum", "build", "upload", "monitor", "bum-ota"],
                 "src/commander_modules.h"),
    "bluepill": ([".gitignore", "platformio.ini", "src/main.cpp", "cmdr.toml", "scripts/stm32_build.py",
                  "bum", "build", "upload", "monitor"], "src/commander_modules.h"),
    "unoq":     ([".gitignore", "CMakeLists.txt", "prj.conf", "app.overlay", "README.md", "src/main.cpp",
                  "cmdr.toml", "build", "flash", "monitor", "bum", "enable-flash-boot",
                  "install-broker", "restore-arduino", "deploy-sbc"], "src/commander_modules.h"),
    "esp32":    ([".gitignore", "CMakeLists.txt", "sdkconfig.defaults", "secrets.h", "main/CMakeLists.txt",
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
    t, mods, _as = cli_mod.read_manifest(root / "cmdr.toml")
    assert t == target
    assert mods == {}


def test_init_rejects_existing_dir(cli_mod, project_dir):
    cli_mod.cmd_init(init_args("pico", name="dup"))
    with pytest.raises(SystemExit):
        cli_mod.cmd_init(init_args("pico", name="dup"))


def test_init_rejects_bad_name(cli_mod, project_dir):
    with pytest.raises(SystemExit):
        cli_mod.cmd_init(init_args("pico", name="bad/name"))


def test_pico_init_configures_cmake(cli_mod, project_dir, monkeypatch):
    """Pico scaffold kicks off a cmake configure (the only target that does)."""
    monkeypatch.setenv("PICO_SDK_PATH", "/opt/pico-sdk")
    monkeypatch.setenv("FREERTOS_KERNEL_PATH", "/opt/FreeRTOS-Kernel")
    cli_mod.cmd_init(init_args("pico"))
    assert any(c[0] == "cmake" for c in project_dir.calls), "pico init should run cmake -B"


def test_pico_init_skips_cmake_without_sdks(cli_mod, project_dir, monkeypatch, capsys):
    """Without the SDK env vars the pico scaffold still succeeds — it skips the
    configure step with instructions instead of dying on a raw CMake error."""
    monkeypatch.delenv("PICO_SDK_PATH", raising=False)
    monkeypatch.delenv("FREERTOS_KERNEL_PATH", raising=False)
    cli_mod.cmd_init(init_args("pico"))
    assert not any(c[0] == "cmake" for c in project_dir.calls)
    assert "Skipping cmake configure" in capsys.readouterr().out
    assert (project_dir.path / "proj" / "CMakeLists.txt").exists()


def test_esp32_layout_has_main_component(cli_mod, project_dir):
    cli_mod.cmd_init(init_args("esp32"))
    root = project_dir.path / "proj"
    # generated file + main.cpp both live in the IDF component dir main/
    assert (root / "main" / "commander_modules.h").exists()
    assert (root / "main" / "main.cpp").exists()
    # no cmake/idf invoked at scaffold time for esp32 (first build does set-target)
    assert not any(c and c[0] in ("cmake", "idf.py") for c in project_dir.calls)


def test_cmake_fetchcontent_form_and_discriminator(cli_mod, project_dir):
    """All CMake targets use FetchContent_MakeAvailable (not the deprecated _Populate).
    esp32/unoq download-only via SOURCE_SUBDIR=include; esp32 carries the IDF_PATH marker
    that the OTA/littlefs codepaths use to tell esp32 from pico (a regression guard for the
    Populate->MakeAvailable migration, which removed the old Populate-vs-MakeAvailable
    discriminator)."""
    for target in ("esp32", "unoq", "pico"):
        name = f"p_{target}"
        cli_mod.cmd_init(init_args(target, name=name))
        cmake = (project_dir.path / name / "CMakeLists.txt").read_text()
        # strip comment lines so a comment that mentions Populate isn't a false positive
        code = "\n".join(l for l in cmake.splitlines() if not l.lstrip().startswith("#"))
        assert "FetchContent_MakeAvailable(commander)" in code, f"{target}: must use MakeAvailable"
        assert "FetchContent_Populate(commander)" not in code, f"{target}: deprecated Populate present"
        if target in ("esp32", "unoq"):
            assert "SOURCE_SUBDIR" in code, f"{target}: download-only needs SOURCE_SUBDIR"
        # IDF_PATH is the esp32 discriminator the OTA/littlefs dispatch relies on
        assert ("IDF_PATH" in code) == (target == "esp32"), f"{target}: IDF_PATH marker mismatch"


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


# ── framework pin ────────────────────────────────────────────────────────────
# A scaffold pins a release TAG, never a branch: a mistake pushed to commander must not reach
# into projects generated last month. Their owners move when they choose, with `cmdr pin` /
# `cmdr pull`. These guard the three places CMake is emitted, which drifted apart before.
CMAKE_TARGETS = ["pico", "pico2", "esp32", "unoq"]


@pytest.mark.parametrize("target", CMAKE_TARGETS)
def test_scaffold_pins_the_release_tag(cli_mod, project_dir, target):
    cli_mod.cmd_init(init_args(target))
    cmake = (project_dir.path / "proj" / "CMakeLists.txt").read_text()
    assert f"GIT_TAG        {cli_mod.FRAMEWORK_TAG}" in cmake or \
           f"GIT_TAG {cli_mod.FRAMEWORK_TAG}" in cmake, \
        f"{target} scaffold does not pin FRAMEWORK_TAG"
    assert "GIT_TAG        main" not in cmake and "GIT_TAG main" not in cmake, \
        f"{target} scaffold still floats on main"


def test_framework_tag_is_a_release_tag(cli_mod):
    tag = cli_mod.FRAMEWORK_TAG
    assert tag not in ("main", "master"), "FRAMEWORK_TAG must be a tag, not a branch"
    assert tag.startswith("v") and tag[1].isdigit(), f"unexpected tag shape: {tag}"


def test_scaffold_pin_satisfies_codegen(cli_mod):
    """cmdr must never scaffold a project it then refuses to generate for.

    FRAMEWORK_TAG (what a new project pins) must be >= MIN_FRAMEWORK_TAG (what
    this cmdr's codegen needs). If codegen starts depending on unreleased
    framework code, MIN_FRAMEWORK_TAG goes up — and this test says the release
    tag has to follow before it can ship."""
    have = cli_mod._parse_release(cli_mod.FRAMEWORK_TAG)
    need = cli_mod._parse_release(cli_mod.MIN_FRAMEWORK_TAG)
    assert have and need, "both tags must be vMAJOR.MINOR releases"
    assert have >= need, (
        f"scaffolds pin {cli_mod.FRAMEWORK_TAG} but codegen needs "
        f"{cli_mod.MIN_FRAMEWORK_TAG} — a fresh project would be refused by "
        f"check_framework_version()")


def test_framework_tag_matches_the_newest_release_tag(cli_mod):
    """FRAMEWORK_TAG must not lag the newest release tag in this repo.

    It is what a fresh `cmdr init` pins, so when it lags, every new project is
    scaffolded onto an older framework than the one being shipped — silently,
    since nothing fails to build. It went stale across two releases before this
    test existed (v1.7 and v1.8 both shipped with it still reading v1.6, so a new
    project would have been pinned to a version with a known radio bug).

    Skipped outside a git checkout: an installed cmdr has no tags to compare to.
    """
    import subprocess
    from pathlib import Path
    repo = Path(cli_mod.__file__).resolve().parents[4]
    if not (repo / ".git").exists():
        pytest.skip("not a git checkout")
    try:
        out = subprocess.run(["git", "-C", str(repo), "tag", "--list", "v*"],
                             capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        pytest.skip("git unavailable")
    tags = [cli_mod._parse_release(t.strip()) for t in out.stdout.splitlines()]
    tags = [t for t in tags if t]
    if not tags:
        pytest.skip("no release tags yet")
    newest = max(tags)
    have = cli_mod._parse_release(cli_mod.FRAMEWORK_TAG)
    # ">=" not "==": during a release the constant is bumped before the tag is
    # cut, and that window is legitimate. What must never happen is lagging.
    assert have >= newest, (
        f"FRAMEWORK_TAG is v{have[0]}.{have[1]} but the newest release tag is "
        f"v{newest[0]}.{newest[1]} — bump it as part of cutting the release, or "
        f"new projects get pinned to the older framework")
