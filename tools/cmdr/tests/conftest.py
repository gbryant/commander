"""Shared fixtures for the cmdr test suite (Tier 1 — see docs/testing.md).

Pure Python, seconds-fast, no toolchains or hardware. The codegen tests call
``generate_modules_file`` directly and compare the emitted ``commander_modules.h``
against checked-in golden snapshots; the scaffolding tests drive ``cmd_init`` /
``cmd_module`` with subprocess + ``input`` stubbed so nothing touches a compiler.
"""
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

# Import the in-tree cmdr without installing it.
_SRC = Path(__file__).resolve().parents[1] / "src"
sys.path.insert(0, str(_SRC))
import cmdr.cli as cli  # noqa: E402

GOLDEN_DIR = Path(__file__).resolve().parent / "golden"


@pytest.fixture
def cli_mod():
    return cli


def resolve_default(default, target):
    """A question default may be a per-target dict; resolve it the way cmd_module does."""
    if isinstance(default, dict):
        return default.get(target, next(iter(default.values())))
    return default


def default_opts(name, target, **overrides):
    """Reproduce the opts dict `cmdr module enable <name>` would store when every
    question is answered with its default — including the int(ans, 0) coercion and
    the feature-flag booleans. Keeps goldens faithful to real enable output and
    fails loudly if a spec's questions/features change shape."""
    spec = cli.MODULE_SPECS[name]
    opts = {}
    for key, _prompt, default in spec["questions"]:
        val = resolve_default(default, target)
        try:
            opts[key] = int(val, 0)
        except (ValueError, TypeError):
            opts[key] = val
    for key, _prompt, fdefault, _flag in spec.get("features", []):
        opts[key] = fdefault
    opts.update(overrides)
    return opts


def build_modules(target, names, overrides=None):
    """{name: default_opts} for a set of enabled modules, with optional per-module
    overrides (e.g. {'aicam': {'transport': 'i2c'}})."""
    overrides = overrides or {}
    return {n: default_opts(n, target, **overrides.get(n, {})) for n in names}


@pytest.fixture
def project_dir(tmp_path, monkeypatch):
    """A throwaway project root: chdir into an empty tmp dir, stub subprocess so a
    scaffold/enable never actually invokes cmake/idf/pio, and pin HOME so WiFi
    credentials resolve to the deterministic placeholder."""
    calls = []

    def fake_run(cmd, *a, **k):
        calls.append(list(cmd) if isinstance(cmd, (list, tuple)) else [cmd])
        class R:  # minimal CompletedProcess stand-in
            returncode = 0
            stdout = ""
            stderr = ""
        return R()

    monkeypatch.setattr(cli.subprocess, "run", fake_run)
    monkeypatch.setattr(cli, "CONFIG_PATH", tmp_path / "_home" / ".cmdr" / "config")
    monkeypatch.chdir(tmp_path)
    return SimpleNamespace(path=tmp_path, calls=calls)
