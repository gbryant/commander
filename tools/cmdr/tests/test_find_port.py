"""find_port.py port selection (Tier 1 — see docs/testing.md).

find_port.py is a *template*: cmdr copies it into every scaffolded project, so
it's exercised here by loading the template file directly rather than importing
an installed module.

The rule these tests pin: two boards can share a USB-serial chip (two CH340s,
say), and VID/PID then can't tell them apart. Picking one silently connects you
to the wrong board and looks like a dead board, so ambiguity must be a loud
failure with a way out — not a guess. The dev scripts poll on exit 1 ("not
enumerated yet") and stop on anything else, so the exit codes are load-bearing.
"""
import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

_TEMPLATE = (Path(__file__).resolve().parents[1]
             / "src" / "cmdr" / "templates" / "find_port.py")

CH340 = (0x1A86, 0x7523)      # matches `esp32` (and the uno clone entry)
PICO_W = (0x2E8A, 0x000A)     # matches `pico`, and reports a serial number


@pytest.fixture
def fp(monkeypatch):
    """The template, loaded fresh with a stubbed serial-port enumerator."""
    spec = importlib.util.spec_from_file_location("find_port_under_test", _TEMPLATE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    monkeypatch.delenv("CMDR_PORT", raising=False)
    return mod


def port(device, vid_pid, serial=None, location=None, description="USB Serial"):
    vid, pid = vid_pid
    return SimpleNamespace(device=device, vid=vid, pid=pid, serial_number=serial,
                           location=location, description=description)


def attach(fp, monkeypatch, *ports):
    monkeypatch.setattr(fp.list_ports, "comports", lambda: list(ports))


def test_single_match_prints_the_port(fp, monkeypatch, capsys):
    attach(fp, monkeypatch, port("/dev/ttyUSB0", CH340))
    fp.find("esp32")
    assert capsys.readouterr().out.strip() == "/dev/ttyUSB0"


def test_absent_board_exits_1_so_scripts_keep_polling(fp, monkeypatch):
    """1 is 'not enumerated yet' — the dev scripts retry on it, quietly."""
    attach(fp, monkeypatch)
    with pytest.raises(SystemExit) as e:
        fp.find("esp32")
    assert e.value.code == 1


def test_ambiguous_match_exits_2_and_names_the_candidates(fp, monkeypatch, capsys):
    """The bug this guards: two CH340 boards, one picked silently, wrong board."""
    attach(fp, monkeypatch,
           port("/dev/ttyUSB0", CH340), port("/dev/ttyUSB1", CH340))
    with pytest.raises(SystemExit) as e:
        fp.find("esp32")
    assert e.value.code == 2
    err = capsys.readouterr().err
    assert "/dev/ttyUSB0" in err and "/dev/ttyUSB1" in err
    assert "CMDR_PORT" in err          # tells you how to resolve it


def test_unknown_board_exits_2(fp, monkeypatch):
    """Not a 'wait for it' condition, so it must not make scripts poll."""
    attach(fp, monkeypatch)
    with pytest.raises(SystemExit) as e:
        fp.find("no-such-board")
    assert e.value.code == 2


def test_env_pin_wins_over_ambiguity(fp, monkeypatch, capsys):
    attach(fp, monkeypatch,
           port("/dev/ttyUSB0", CH340), port("/dev/ttyUSB1", CH340))
    monkeypatch.setenv("CMDR_PORT", "/dev/ttyUSB1")
    fp.find("esp32")
    assert capsys.readouterr().out.strip() == "/dev/ttyUSB1"


def test_toml_serial_pin_selects_that_board(fp, monkeypatch, tmp_path, capsys):
    (tmp_path / "cmdr.toml").write_text('target = "pico"\nserial = "SN-B"\n')
    monkeypatch.chdir(tmp_path)
    attach(fp, monkeypatch,
           port("/dev/ttyACM0", PICO_W, serial="SN-A"),
           port("/dev/ttyACM1", PICO_W, serial="SN-B"))
    fp.find("pico")
    assert capsys.readouterr().out.strip() == "/dev/ttyACM1"


def test_toml_port_pin_used_when_devices_report_no_serial(fp, monkeypatch, tmp_path, capsys):
    """CH340 clones report no serial, so the path is the only handle they have."""
    (tmp_path / "cmdr.toml").write_text('target = "esp32"\nport = "/dev/ttyUSB1"\n')
    monkeypatch.chdir(tmp_path)
    attach(fp, monkeypatch,
           port("/dev/ttyUSB0", CH340), port("/dev/ttyUSB1", CH340))
    fp.find("esp32")
    assert capsys.readouterr().out.strip() == "/dev/ttyUSB1"


def test_report_recommends_serial_only_when_one_exists(fp, monkeypatch):
    """Don't tell someone to pin a serial their hardware doesn't report."""
    attach(fp, monkeypatch,
           port("/dev/ttyUSB0", CH340), port("/dev/ttyUSB1", CH340))
    no_serial = fp._ambiguity_report("esp32", fp._matching("esp32"))
    assert "serial =" not in no_serial
    assert 'port   = "/dev/ttyUSB0"' in no_serial

    attach(fp, monkeypatch,
           port("/dev/ttyACM0", PICO_W, serial="SN-A"),
           port("/dev/ttyACM1", PICO_W, serial="SN-B"))
    with_serial = fp._ambiguity_report("pico", fp._matching("pico"))
    assert 'serial = "SN-A"' in with_serial


def test_find_for_project_reads_target_and_stays_usable(fp, monkeypatch, tmp_path, capsys):
    """Library path is lenient — interactive tools keep working — but warns."""
    (tmp_path / "cmdr.toml").write_text('target = "esp32"\n\n[autostart]\nlines = []\n')
    monkeypatch.chdir(tmp_path)
    attach(fp, monkeypatch, port("/dev/ttyUSB0", CH340))
    assert fp.find_for_project() == "/dev/ttyUSB0"

    attach(fp, monkeypatch,
           port("/dev/ttyUSB0", CH340), port("/dev/ttyUSB1", CH340))
    assert fp.find_for_project() == "/dev/ttyUSB0"
    assert "Ambiguous" in capsys.readouterr().err


def test_section_headers_do_not_leak_into_top_level_keys(fp, tmp_path, monkeypatch):
    """`port` under [autostart] is not a top-level pin."""
    (tmp_path / "cmdr.toml").write_text(
        'target = "esp32"\n\n[autostart]\nport = "/dev/wrong"\n')
    monkeypatch.chdir(tmp_path)
    assert fp._cmdr_toml_keys(tmp_path) == {"target": "esp32"}
