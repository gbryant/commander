"""The gdb helpers read private members by name — this checks the names are real.

`scripts/gdb/commander.py` reaches into `CommandRegistry` and `UartTransport`
private fields through DWARF. That is the right call (no accessors added just
for debugging), but it means a field rename breaks the helpers **silently**:
nothing fails to compile, and the breakage only shows up the next time someone
is mid-debug and least wants a surprise. The roadmap called this out as the
reason these need a regression test.

A gdb-driven test is the fuller check, but gdb with Python support isn't
present everywhere (Arm's own macOS toolchain builds omit it), so it can't be
the gate. This asserts the contract that actually rots: every field name the
script reads still exists in the header it reads it from.
"""
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "gdb" / "commander.py"


def _header(rel):
    p = ROOT / rel
    assert p.exists(), f"{rel} moved — update this test and the gdb helpers"
    return p.read_text()


def test_the_script_exists_where_swd_sh_looks_for_it():
    """swd.sh loads $(dirname swd.sh)/gdb/commander.py — a rename here silently
    turns the helpers off rather than erroring."""
    assert SCRIPT.exists()
    swd = (ROOT / "scripts" / "swd.sh").read_text()
    assert "gdb/commander.py" in swd


@pytest.mark.parametrize("field", ["_commands", "_count", "_dropped", "_firstDropped"])
def test_command_registry_fields_still_exist(field):
    assert field in _header("core/CommandRegistry.h"), \
        f"CommandRegistry::{field} is gone — scripts/gdb/commander.py reads it"
    assert field in SCRIPT.read_text()


@pytest.mark.parametrize("field", ["_tickers", "_tickCount", "_tickDropped"])
def test_uart_transport_fields_still_exist(field):
    assert field in _header("transport/uart/UartTransport.h"), \
        f"UartTransport::{field} is gone — scripts/gdb/commander.py reads it"
    assert field in SCRIPT.read_text()


@pytest.mark.parametrize("member", ["name", "help", "i2c_id", "ctx"])
def test_command_struct_members_still_exist(member):
    """cmdr-commands prints these, and cmdr-modules groups by ctx."""
    struct = re.search(r"struct Command\s*\{(.*?)\};",
                       _header("core/CommandRegistry.h"), re.S)
    assert struct, "struct Command not found in core/CommandRegistry.h"
    assert member in struct.group(1), \
        f"Command::{member} is gone — scripts/gdb/commander.py reads it"


def test_the_types_the_script_searches_for_are_the_real_class_names():
    """_resolve() finds objects by matching these type names in `info variables`,
    so a class rename would leave it finding nothing at all."""
    assert "class CommandRegistry" in _header("core/CommandRegistry.h")
    assert "class UartTransport" in _header("transport/uart/UartTransport.h")
    body = SCRIPT.read_text()
    assert '_resolve("CommandRegistry"' in body
    assert '_resolve("UartTransport"' in body


def test_panic_hook_is_still_the_symbol_the_helper_looks_for():
    assert "commander_on_panic" in _header("core/CommandRegistry.cpp")
    assert "commander_on_panic" in SCRIPT.read_text()


def test_every_command_is_registered():
    """Each gdb.Command subclass must be instantiated, or it silently never
    becomes available inside gdb."""
    body = SCRIPT.read_text()
    classes = re.findall(r"^class (\w+)\(gdb\.Command\)", body, re.M)
    assert classes, "no gdb.Command subclasses found"
    for c in classes:
        assert re.search(rf"^{c}\(\)$", body, re.M), f"{c} is defined but never instantiated"
