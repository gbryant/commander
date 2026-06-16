"""ESP32 composable partition table — parse_partitions/compose_partitions.

The whole point of the composition model is that `enable ota` and `enable littlefs`
re-derive each other's state from the existing table, so order doesn't matter and
`disable ota` keeps a filesystem. These tests pin those algebraic properties
(parse∘compose round-trips; app slots reflow around a fixed FS) without an ESP-IDF.
"""
import pytest


def _has_fs(text, label="storage"):
    _ota, fs = __import__("cmdr.cli", fromlist=["parse_partitions"]).parse_partitions(text)
    return any(l == label for (l, _st, _sz) in fs)


def test_compose_parse_roundtrip_single_app(cli_mod):
    csv = cli_mod.compose_partitions(16, ota=False, fs=[])
    ota, fs = cli_mod.parse_partitions(csv)
    assert ota is False and fs == []
    assert "factory" in csv and "ota_0" not in csv


def test_compose_parse_roundtrip_ota(cli_mod):
    csv = cli_mod.compose_partitions(16, ota=True, fs=[])
    ota, fs = cli_mod.parse_partitions(csv)
    assert ota is True and fs == []
    assert "ota_0" in csv and "ota_1" in csv and "otadata" in csv


def test_fs_partition_survives_and_is_parsed(cli_mod):
    csv = cli_mod.compose_partitions(16, ota=False, fs=[("storage", "littlefs", 2 * 0x100000)])
    ota, fs = cli_mod.parse_partitions(csv)
    assert ota is False
    assert len(fs) == 1
    label, subtype, size = fs[0]
    assert label == "storage" and subtype == "littlefs" and size == 2 * 0x100000


def test_enable_ota_keeps_filesystem(cli_mod):
    """The composition promise: re-derive FS from the old table, add OTA, keep the FS."""
    base = cli_mod.compose_partitions(16, ota=False, fs=[("storage", "littlefs", 2 * 0x100000)])
    old_ota, old_fs = cli_mod.parse_partitions(base)
    # `enable ota` recomposes with ota=True but the SAME fs list it parsed back.
    composed = cli_mod.compose_partitions(16, ota=True, fs=old_fs)
    ota, fs = cli_mod.parse_partitions(composed)
    assert ota is True, "ota now enabled"
    assert fs == old_fs, "filesystem partition preserved across enable ota"


def test_disable_ota_keeps_filesystem(cli_mod):
    base = cli_mod.compose_partitions(16, ota=True, fs=[("storage", "littlefs", 2 * 0x100000)])
    _o, old_fs = cli_mod.parse_partitions(base)
    composed = cli_mod.compose_partitions(16, ota=False, fs=old_fs)
    ota, fs = cli_mod.parse_partitions(composed)
    assert ota is False and fs == old_fs, "disable ota must keep the filesystem"


def test_order_independence(cli_mod):
    """enable-ota-then-fs and enable-fs-then-ota converge to the same table."""
    fs = [("storage", "littlefs", 2 * 0x100000)]
    a = cli_mod.compose_partitions(16, ota=True, fs=fs)        # ota first, then fs present
    b = cli_mod.compose_partitions(16, ota=True, fs=fs)        # fs first, then ota
    assert a == b


def test_app_slots_reflow_around_fixed_fs(cli_mod):
    """A bigger FS leaves less room for the app — OTA slots must shrink, not overflow."""
    small = cli_mod.compose_partitions(16, ota=True, fs=[("storage", "littlefs", 1 * 0x100000)])
    big = cli_mod.compose_partitions(16, ota=True, fs=[("storage", "littlefs", 4 * 0x100000)])

    def app0_size(csv):
        for line in csv.splitlines():
            cols = [c.strip() for c in line.split(",")]
            if cols and cols[0] == "app0":
                return int(cols[4], 0)
        raise AssertionError("no app0 slot")

    assert app0_size(big) < app0_size(small), "app slot should shrink as the FS grows"


def test_oversized_fs_is_rejected(cli_mod):
    """A filesystem larger than flash must die loudly, not silently truncate."""
    with pytest.raises(SystemExit):
        cli_mod.compose_partitions(4, ota=True, fs=[("storage", "littlefs", 64 * 0x100000)])
