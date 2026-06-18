"""Honest-menu gating — _module_supported() must offer a module only where its HAL
backs it, so cmdr never lets you enable a module that silently won't work.

The Uno Q is the sharp case: its Zephyr HAL backs only console/IR so far, so its menu
is a hardcoded allowlist (UNOQ_MODULES) until the HAL grows (roadmap #4). These tests
pin that contract and the per-platform `platforms` lists.
"""
import pytest

ALL_TARGETS = ["uno", "r4", "pico", "pico2", "esp32", "bluepill", "unoq"]


def test_system_supported_everywhere(cli_mod):
    for t in ALL_TARGETS:
        assert cli_mod._module_supported("system", t)


def test_unoq_allowlist_is_honest(cli_mod):
    """Only system + ir on unoq; the I2C/GPIO sensor modules are gated off."""
    assert cli_mod.UNOQ_MODULES == {"system", "ir"}
    for name in cli_mod.MODULE_SPECS:
        supported = cli_mod._module_supported(name, "unoq")
        assert supported == (name in cli_mod.UNOQ_MODULES), (
            f"{name} unoq support ({supported}) disagrees with UNOQ_MODULES"
        )


def test_portable_modules_offered_on_all_real_targets(cli_mod):
    """platforms=None means portable — offered everywhere except the unoq allowlist."""
    for name, spec in cli_mod.MODULE_SPECS.items():
        if spec["platforms"] is not None:
            continue
        for t in ALL_TARGETS:
            expect = True if t != "unoq" else name in cli_mod.UNOQ_MODULES
            assert cli_mod._module_supported(name, t) == expect, f"{name} on {t}"


def test_platform_gated_modules_match_their_spec(cli_mod):
    """A platform-gated module is supported exactly on its declared platforms
    (intersected with the unoq allowlist)."""
    for name, spec in cli_mod.MODULE_SPECS.items():
        plats = spec["platforms"]
        if plats is None:
            continue
        for t in ALL_TARGETS:
            expect = t in plats
            if t == "unoq":
                expect = name in cli_mod.UNOQ_MODULES
            assert cli_mod._module_supported(name, t) == expect, f"{name} on {t} (spec {plats})"


@pytest.mark.parametrize("name,target", [
    ("ipstube", "pico"), ("ws2812", "pico"), ("aicam", "r4"),   # esp32-only
    ("controller", "esp32"), ("locomotion", "r4"),               # pico-only
    ("loco-bridge", "pico"), ("roomba", "pico"),                 # r4-only
    ("wifi", "uno"), ("wifi", "bluepill"),                       # wifi needs a runner hook
])
def test_unsupported_combinations_are_gated(cli_mod, name, target):
    assert not cli_mod._module_supported(name, target), f"{name} should be gated off {target}"


def test_every_optional_module_supported_somewhere(cli_mod):
    """No module is dead — each is offered on at least one target."""
    for name in cli_mod.MODULE_SPECS:
        assert any(cli_mod._module_supported(name, t) for t in ALL_TARGETS), f"{name} unreachable"
