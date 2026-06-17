#!/bin/bash
# Tier 2/3 — generated-project compile smoke (docs/testing.md).
#
# Golden/codegen tests (tests/run.sh) prove cmdr emits the *expected text*; they
# cannot prove that text *compiles*. This does: for a matrix of (board, module-set)
# it `cmdr init`s a throwaway project, points it at THIS local commander checkout
# (cmdr link / a symlink:// lib_dep — so it tests the working tree, not GitHub main),
# enables a representative module set, and invokes the real generated build script.
#
# It is toolchain-detecting and skip-with-notice — never all-or-nothing. Every
# toolchain we ship builds on the Mac (Uno/R4/Bluepill via PlatformIO; Pico/Pico 2 W
# via CMake+Pico SDK; ESP32 via ESP-IDF after `esp`; Uno Q via west+gnuarmemb). What a
# row can't do locally is *flash* (e.g. the Uno Q needs an adb-reachable board) — but
# it still *compiles* here. Missing toolchain (or an optional dep like BLUEPAD32_PATH)
# => SKIP with a reason, not a failure.
#
# Usage:
#   tests/build-matrix.sh                # Tier 2: one representative config per board
#   tests/build-matrix.sh --full         # Tier 3: every config in the matrix
#   tests/build-matrix.sh --list         # print the matrix and what each row needs
#   tests/build-matrix.sh pico r4        # only these boards
#
# Build dirs are reused under $CMDR_MATRIX_CACHE (default a stable tmp dir) for speed.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMDR=(env "PYTHONPATH=$ROOT/tools/cmdr/src" python3 -m cmdr.cli)
CACHE="${CMDR_MATRIX_CACHE:-${TMPDIR:-/tmp}/cmdr-build-matrix}"

# ── matrix ──────────────────────────────────────────────────────────────────────
# Each row: target | label | tier(2|3) | modules(space-sep, '-' for none) | needs(csv)
# tier 2 rows are the default representative set; tier 3 adds the rest (--full).
# `needs` are optional deps beyond the base toolchain (a missing one => SKIP, not FAIL).
MATRIX=(
    "pico|base|2|-|"
    "pico|sensors|2|ir compass i2c sonar|"
    "pico|controller|3|controller|BLUEPAD32_PATH"
    "pico|serial_monitor|3|serial_monitor|"
    "pico|loco|3|locomotion wifi|"
    "pico2|base|3|-|"
    "pico2|controller|3|controller locomotion|BLUEPAD32_PATH"
    "uno|base|3|-|"
    "uno|ir|2|ir compass sonar|"
    "r4|base|3|-|"
    "r4|wifi_ir|2|wifi ir|"
    "r4|loco_bridge|3|loco-bridge|"
    "bluepill|base|2|-|"
    "bluepill|sensors|3|ds1302 sonar|TINYUSB"
    "esp32|base|2|-|"
    "esp32|display|3|ws2812 ds1302|"
    "esp32|sensors|3|wifi ina219|"
    "unoq|base|2|-|"
    "unoq|ir|3|ir|"
)

# ── toolchain probes: echo "" if available, else a skip reason ────────────────────
# ESP-IDF and Zephyr aren't on the bare PATH — they're activated by an env script (the
# `esp` alias sources export.sh; the unoq `build` script sources the Zephyr venv). So we
# probe for those activators, not for idf.py/west on PATH, and the build step activates them.

# Echo a sourceable ESP-IDF export.sh (mirrors the `esp` alias), or empty.
idf_export() {
    local p
    for p in "${IDF_EXPORT:-}" "${IDF_PATH:-}/export.sh" \
             "$HOME/u-developer/esp-idf/export.sh" "$HOME/esp/esp-idf/export.sh" "$HOME/esp-idf/export.sh"; do
        [ -n "$p" ] && [ -f "$p" ] && { echo "$p"; return; }
    done
}
# Echo the Zephyr venv activate script the unoq build sources, or empty.
zephyr_venv() {
    local v="${ZEPHYR_VENV:-$HOME/zephyrproject/.venv}/bin/activate"
    [ -f "$v" ] && echo "$v"
}

probe_pico()  { command -v cmake >/dev/null || { echo "no cmake"; return; }
                [ -d "${PICO_SDK_PATH:-/nonexistent}" ] || { echo "PICO_SDK_PATH unset/missing"; return; }; echo ""; }
probe_pio()   { command -v pio >/dev/null && echo "" || echo "no pio (PlatformIO)"; }
probe_esp32() { command -v idf.py >/dev/null && { echo ""; return; }
                [ -n "$(idf_export)" ] && { echo ""; return; }
                echo "no idf.py and no esp-idf export.sh (run 'esp', or set IDF_EXPORT)"; }
probe_unoq()  { command -v west >/dev/null || [ -n "$(zephyr_venv)" ] || { echo "no west / Zephyr venv (\$ZEPHYR_VENV)"; return; }
                { command -v arm-none-eabi-gcc >/dev/null || ls -d /Applications/ArmGNUToolchain/*/ >/dev/null 2>&1; } \
                    || { echo "no Arm GNU toolchain (gnuarmemb)"; return; }; echo ""; }

probe_target() {
    case "$1" in
        pico|pico2) probe_pico ;;
        uno|r4|bluepill) probe_pio ;;
        esp32) probe_esp32 ;;
        unoq) probe_unoq ;;
        *) echo "unknown target" ;;
    esac
}

needs_ok() {  # "$1" csv of required env/dirs; echo "" if all present, else reason
    local n
    for n in ${1//,/ }; do
        [ -z "$n" ] && continue
        case "$n" in
            BLUEPAD32_PATH) [ -d "${BLUEPAD32_PATH:-/nonexistent}" ] || { echo "BLUEPAD32_PATH unset"; return; } ;;
            TINYUSB) [ -n "${TINYUSB_PATH:-}" ] || [ -d "${PICO_SDK_PATH:-/nonexistent}/lib/tinyusb" ] \
                        || { echo "no TinyUSB ($TINYUSB_PATH / PICO_SDK/lib/tinyusb)"; return; } ;;
        esac
    done
    echo ""
}

# ── link a generated project at the LOCAL commander tree ──────────────────────────
link_local() {  # $1 = project dir, $2 = target
    local dir="$1" target="$2"
    if [ -f "$dir/platformio.ini" ]; then
        # Replace the GitHub lib_dep with a symlink:// at this checkout.
        python3 - "$dir/platformio.ini" "$ROOT" <<'PY'
import re, sys
ini, root = sys.argv[1], sys.argv[2]
t = open(ini).read()
t = re.sub(r"https://github\.com/\S+/commander\.git", f"symlink://{root}", t)
open(ini, "w").write(t)
PY
    else
        ( cd "$dir" && "${CMDR[@]}" link "$ROOT" >/dev/null )
    fi
}

build_project() {  # $1 = project dir, $2 = target ; returns build exit
    local dir="$1" target="$2"
    case "$target" in
        pico)     ( cd "$dir" && cmake -B build-pico -S . -DPICO_BOARD=pico_w >/dev/null 2>&1 && cmake --build build-pico -j ) ;;
        pico2)    ( cd "$dir" && cmake -B build-pico2 -S . -DPICO_BOARD=pico2_w >/dev/null 2>&1 && cmake --build build-pico2 -j ) ;;
        # bluepill's stm32_build.py resolves commander via $COMMANDER_PATH (its blessed
        # local-dev path) — a symlink:// lib_dep isn't where it looks. uno/r4 ignore it.
        uno|r4|bluepill) ( cd "$dir" && COMMANDER_PATH="$ROOT" pio run ) ;;
        # esp32: activate the IDF env (like `esp`) if idf.py isn't already on PATH, then run
        # the generated ./build (it does set-target on first run). unoq's ./build sources its
        # own Zephyr venv. Both compile here; only *flashing* the Uno Q needs the board.
        esp32)    ( cd "$dir"
                    if ! command -v idf.py >/dev/null; then exp="$(idf_export)"; [ -n "$exp" ] && . "$exp" >/dev/null 2>&1; fi
                    ./build ) ;;
        unoq)     ( cd "$dir" && ./build ) ;;
    esac
}

# ── arg parsing ───────────────────────────────────────────────────────────────────
FULL=0; LIST=0; ONLY=()
for a in "$@"; do
    case "$a" in
        --full) FULL=1 ;;
        --list) LIST=1 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) ONLY+=("$a") ;;
    esac
done
want_target() { [ ${#ONLY[@]} -eq 0 ] && return 0; local t; for t in "${ONLY[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }

if [ "$LIST" = 1 ]; then
    printf "%-9s %-13s %-4s %-28s %s\n" TARGET LABEL TIER MODULES NEEDS
    for row in "${MATRIX[@]}"; do IFS='|' read -r t l tier mods needs <<< "$row"
        printf "%-9s %-13s %-4s %-28s %s\n" "$t" "$l" "$tier" "$mods" "$needs"; done
    exit 0
fi

mkdir -p "$CACHE"
echo "build-matrix: $([ "$FULL" = 1 ] && echo 'Tier 3 (full)' || echo 'Tier 2 (representative)')  cache=$CACHE"
echo

declare -i n_pass=0 n_skip=0 n_fail=0
declare -a FAILED=()

for row in "${MATRIX[@]}"; do
    IFS='|' read -r target label tier mods needs <<< "$row"
    want_target "$target" || continue
    [ "$FULL" = 0 ] && [ "$tier" != "2" ] && continue

    tag="$target/$label"
    reason="$(probe_target "$target")"
    if [ -n "$reason" ]; then echo "SKIP $tag — $reason"; n_skip+=1; continue; fi
    reason="$(needs_ok "$needs")"
    if [ -n "$reason" ]; then echo "SKIP $tag — $reason"; n_skip+=1; continue; fi

    # mtx_ prefix: a bare "pico_base" collides with the Pico SDK's reserved pico_base
    # target namespace, and "pico_*" names are risky in general.
    name="mtx_${target}_${label}"
    proj="$CACHE/$name"
    log="$CACHE/$name.log"
    rm -rf "$proj"
    echo -n "BUILD $tag ... "

    # Scaffold + link-local + enable modules in a subshell so `set -e` stays scoped.
    if (
        set -e
        cd "$CACHE"
        "${CMDR[@]}" init "$target" "$name"
        link_local "$proj" "$target"
        if [ "$mods" != "-" ]; then
            for m in $mods; do ( cd "$proj" && yes '' | "${CMDR[@]}" module enable "$m" ); done
        fi
    ) >"$log" 2>&1; then :; else
        echo "FAIL (scaffold/enable — see $log)"; n_fail+=1; FAILED+=("$tag"); continue
    fi

    if build_project "$proj" "$target" >>"$log" 2>&1; then
        echo "PASS"; n_pass+=1
    else
        echo "FAIL (compile — see $log)"; n_fail+=1; FAILED+=("$tag")
    fi
done

echo
echo "────────────────────────────────────────────────────────"
echo "build-matrix: $n_pass passed, $n_skip skipped, $n_fail failed"
if [ "$n_fail" -gt 0 ]; then printf '  FAIL: %s\n' "${FAILED[@]}"; exit 1; fi
exit 0
