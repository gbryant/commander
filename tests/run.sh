#!/bin/bash
# Consolidated host test gate — Tier 0 (portable C++) + Tier 1 (cmdr Python).
# Pure host: no cross-compiler, no hardware. Runs in seconds. This is the
# pre-commit gate. The slow, toolchain-dependent compile matrix lives in
# tests/build-matrix.sh (Tier 2/3). See docs/testing.md.
#
# Usage:
#   tests/run.sh            # everything (Tier 0 C++ + Tier 1 pytest)
#   tests/run.sh cpp        # Tier 0 only
#   tests/run.sh py         # Tier 1 only
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
WHAT="${1:-all}"
CXXFLAGS="-std=c++17 -Wall -Wextra"
INC=(-I"$ROOT" -I"$ROOT/include")

run_cpp() {
    local TMP; TMP="$(mktemp -d)"
    local fails=0

    # Each entry: "label | output-binary | extra-flags | source files (space-sep)"
    # core/CommandRegistry.cpp is linked where the registry is exercised.
    local cases=(
        "ChannelCodec|test_codec||transport/channels/tests/test_codec.cpp"
        "ChannelTransport|test_transport|-DMAX_COMMANDS=16|transport/channels/tests/test_transport.cpp core/CommandRegistry.cpp"
        "ChannelBusRunner|test_runner|-DMAX_COMMANDS=16|transport/channels/tests/test_runner.cpp transport/channels/ChannelBusRunner.cpp core/CommandRegistry.cpp"
        "NecDecoder/SonyDecoder|test_nec||modules/ir/tests/test_nec.cpp"
        "CommandRegistry/Writer/SystemModule|test_registry|-DMAX_COMMANDS=16|core/tests/test_registry.cpp core/CommandRegistry.cpp"
        "DriveMixer/LocoProtocol|test_drivemixer||modules/locomotion/tests/test_drivemixer.cpp"
        "ControllerCalibration|test_calibration||modules/controller/tests/test_calibration.cpp"
    )

    for c in "${cases[@]}"; do
        IFS='|' read -r label bin flags srcs <<< "$c"
        echo "== $label =="
        local srcpaths=()
        for s in $srcs; do srcpaths+=("$ROOT/$s"); done
        if ! $CXX $CXXFLAGS $flags "${INC[@]}" "${srcpaths[@]}" -o "$TMP/$bin"; then
            echo "FAIL ($label did not compile)"; fails=$((fails+1)); echo; continue
        fi
        if ! "$TMP/$bin"; then fails=$((fails+1)); fi
        echo
    done

    # The Python broker PTY-loopback test (part of the channel-bus suite).
    if command -v python3 >/dev/null 2>&1; then
        echo "== broker (Python PTY loopback) =="
        if ! TMPDIR="$TMP" python3 "$ROOT/transport/channels/broker/test_broker.py"; then fails=$((fails+1)); fi
        echo

        # Codec<->broker byte-compat guard: the real C codec vs the broker's Python
        # port, byte-for-byte both directions. Catches a silent MCU/broker drift.
        echo "== codec<->broker byte-compat =="
        if $CXX $CXXFLAGS "${INC[@]}" "$ROOT/transport/channels/tests/codec_harness.cpp" -o "$TMP/codec_harness"; then
            if ! python3 "$ROOT/transport/channels/tests/test_codec_compat.py" "$TMP/codec_harness"; then fails=$((fails+1)); fi
        else
            echo "FAIL (codec_harness did not compile)"; fails=$((fails+1))
        fi
        echo
    else
        echo "SKIP broker PTY loopback + codec compat (python3 missing)"; echo
    fi

    rm -rf "$TMP"
    return $fails
}

run_py() {
    echo "== cmdr golden/codegen tests (pytest) =="
    if ! command -v python3 >/dev/null 2>&1; then
        echo "SKIP cmdr tests (python3 missing)"; return 0
    fi
    # Prefer pytest; fall back to a clear notice (the suite is pytest-based).
    if python3 -c "import pytest" >/dev/null 2>&1; then
        ( cd "$ROOT/tools/cmdr" && python3 -m pytest -q )
        return $?
    else
        echo "SKIP cmdr tests (pytest missing — 'pip install pytest')"
        return 0
    fi
}

rc=0
case "$WHAT" in
    cpp) run_cpp || rc=$? ;;
    py)  run_py  || rc=$? ;;
    all)
        run_cpp || rc=$?
        echo "────────────────────────────────────────────────────────"
        run_py || rc=$?
        ;;
    *) echo "usage: tests/run.sh [all|cpp|py]"; exit 2 ;;
esac

echo "────────────────────────────────────────────────────────"
if [ "$rc" -eq 0 ]; then echo "TIER 0/1 GREEN"; else echo "TIER 0/1 FAILURES ($rc suite(s))"; fi
exit $rc
