# Design note: a test suite + local build-test system

**Status:** IMPLEMENTED (2026-06-16) — all four tiers landed. Captured 2026-06-15 as the detailed
plan behind roadmap #1 ("Tests + CI for `cmdr` and a platform build matrix"); the original design
text is kept below for context, with a "what landed" note at the end.

## How to run (quick reference)

```bash
tests/run.sh                 # Tier 0 + Tier 1 — host C++ + cmdr pytest. The pre-commit gate (seconds).
tests/run.sh cpp             # Tier 0 only (no python needed)
tests/run.sh py              # Tier 1 only (needs: pip install pytest)
tests/build-matrix.sh        # Tier 2 — one representative config per board (toolchain-detecting)
tests/build-matrix.sh --full # Tier 3 — every config in the matrix
tests/build-matrix.sh --list # show the matrix + what each row needs
tests/build-matrix.sh pico   # only the given board(s)

# regenerate the cmdr golden snapshots after an intended codegen change:
CMDR_UPDATE_GOLDEN=1 python3 -m pytest tools/cmdr/tests/test_codegen_golden.py
```

commander has **three very different things that can regress**, and they need different test
strategies running at very different speeds:

1. **Portable C++ logic** — `core/`, `modules/`, the channel codec/decoders. Pure, host-compilable.
2. **The `cmdr` tool** — ~2,300 lines of Python codegen/scaffolding, currently **zero tests**. The
   real bugs this past stretch (a duplicate include, the unoq IR-branch wiring, a private-repo curl
   404, stdout buffering, a `build/`-dir collision) all lived here and were caught only by
   hand-scaffolding on real hardware.
3. **Whether 6+ toolchains still compile the generated code** — Uno/R4/Bluepill (PlatformIO),
   Pico/Pico 2 W (CMake + Pico SDK), ESP32 (ESP-IDF), Uno Q (Zephyr/west). Codegen can be *valid*
   and still not *compile*.

A single test type can't cover all three. The plan is a pyramid, fast → slow.

## The pyramid

### Tier 0 — host unit tests for portable C++ *(seconds, no toolchain)*

The pattern already exists: `transport/channels/tests/run.sh` and `modules/ir/tests/run.sh` compile
pure C++ natively and run assertions. **Consolidate** these under one `tests/run.sh` (or `make
test`) and **broaden** coverage to the portable logic that has bitten or easily could:

- COBS `channel_encode` / `ChannelReader` (incl. embedded zeros, overflow resync, >1-frame payloads)
- `NecDecoder` / `SonyDecoder` (jitter, repeat, noise — partly there already)
- `CommandRegistry::dispatch`, `Writer`, `SystemModule`
- `DriveMixer` (two-zone curve, ramping, spin), `ControllerCalibration` (re-center/rescale/deadzone)

This is the instant feedback loop — it catches the Sony-decoder / volatile-struct-assignment class
of bug at compile-or-run, with no cross-compiler in the way.

### Tier 1 — `cmdr` Python tests *(seconds)* — **the highest-leverage gap**

A `pytest` suite under `tools/cmdr/tests/`. The core technique is **golden-file codegen tests**:
for a matrix of `(target, module-set)`, call `generate_modules_file` and assert the emitted
`commander_modules.h` matches a **checked-in snapshot**. Snapshots are updated deliberately; an
*unexpected* diff is the regression signal. This directly pins the codegen bugs from this stretch:

- the duplicate-include bug in `generate_modules_file`
- the unoq IR branch (`ZephyrIRModule` + `ChannelPublisher` wiring, `commander_on_channel_bus_ready`
  vs `commander_on_uart_ready`)
- the **no-designated-initializers** and **CMD-macro-comma** rules — assert these as lint checks
  over generated output (they're invariants, so a test should enforce them, not a code reviewer)

Plus the scaffolding surface:

- `cmdr init <board>` produces the expected file set per board
- module enable/disable **idempotence**; the honest-menu gating (`_module_supported`,
  `UNOQ_MODULES`); target-aware tool install/remove (`unoq_tools` vs the pyserial tools)

The golden files double as **living documentation** of exactly what each module emits.

### Tier 2 — generated-project compile smoke *(minutes, needs toolchains)* — **the real net**

Golden tests prove the codegen emits the *expected text*; they cannot prove that text **compiles**.
So: `cmdr init` a throwaway project per board into a temp dir, enable a representative module set,
and **actually invoke its build script**. This is the only tier that catches "valid-looking codegen
that doesn't compile" — and it would have caught the `build/`-dir collision (it runs the real
generated script) and any header that compiles standalone but not when unity-included.

### Tier 3 — full local build matrix *(no CI)*

Tier 2 exhaustively: all boards × representative module combinations. **No GitHub Actions / no
containers** — this runs locally on the Mac, because every toolchain we ship is reachable here:
Uno/R4/Bluepill (PlatformIO), Pico/Pico 2 W (CMake + Pico SDK), ESP32 (ESP-IDF after `esp`), and
**Uno Q/Zephyr builds on the Mac too** via `ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb` against the
standalone Arm GNU Toolchain 14.2 (`/Applications/ArmGNUToolchain/14.2.rel1`) — no Zephyr SDK, which
has no Intel-macOS toolchain. So Tier 3 is just `build-matrix.sh` with the full config list instead
of one representative set; same skip-with-notice mechanics (e.g. the Uno Q row still needs an
adb-reachable board to *flash*, but it **compiles** locally). Optionally matrix-build the example
consumer repos (`cmdr-robot`, `cmdr-oi-bridge`, …) so their adoption path is guarded too.

## The local build-test system (mechanics)

All six toolchains build on the Mac — including Zephyr/Uno Q, via `gnuarmemb` + the standalone Arm
GNU Toolchain 14.2 (the Zephyr SDK has no Intel-macOS chain, so we don't use the SDK). ESP-IDF just
needs `esp` sourced first. The remaining constraint is *flashing*, not compiling: the Uno Q is
flashed over an adb/ssh-reachable board, so its row can compile locally but only flashes when the
board is up. So the local runner must be **toolchain-detecting and skip-with-notice**, never
all-or-nothing:

- **`tests/run.sh`** → Tier 0 + Tier 1 only. No toolchains, runs in seconds. **This is the
  pre-commit gate** — everything that doesn't need a cross-compiler.
- **`tests/build-matrix.sh`** → Tier 2 (and Tier 3 with the full config list). Probes what's
  available (`pio`; `cmake` + `PICO_SDK_PATH`; `idf.py` after `esp`; `west` + the gnuarmemb chain
  for Zephyr), **compiles** every config it can, and prints `SKIP <cfg> (toolchain missing)` for the
  rest. Flashing the Uno Q still needs an adb/ssh-reachable board, but compiling it does not.
  **Pre-push / on-demand**, minutes. Reuses per-board build dirs for speed.
- **Codec↔broker byte-compat guard** (call this out specifically): a test that compiles a tiny C
  harness emitting `channel_encode` output (embedded zeros + a multi-frame payload), pipes it to the
  Python broker's deframer, and asserts a **byte-for-byte round-trip both directions**. The MCU
  codec and the broker drifting apart is a *silent* failure (mismatched frames just vanish), so this
  belongs in the permanent suite, not the one-off manual cross-check it is today. See
  `docs/channels-first-class.md` Phase E.

### How the tiers map to real bugs

| Bug (this stretch)                         | Caught by |
|--------------------------------------------|-----------|
| Sony-only-NEC decoder, volatile struct `=` | Tier 0    |
| Duplicate include, unoq IR-branch wiring   | Tier 1 (golden) |
| `build/`-dir collision, won't-compile codegen | Tier 2 |
| Private-repo curl 404 in `install-broker`  | Tier 1 lint (generated script must not curl a private raw URL) / Tier 2 |
| stdout block-buffering in `ir_lookup.py`   | a small piped-stdout smoke test (assert a line appears before exit) |
| MCU codec ↔ Python broker drift            | codec↔broker byte-compat guard |

## Where to start

**Tier 1 (`cmdr` golden tests) first.** It's the biggest currently-uncovered surface, it's
seconds-fast, it needs no toolchains, and it pins exactly the codegen-regression class that's been
the recurring papercut. Stand up `pytest` + golden `commander_modules.h` snapshots for the configs
actually shipped (uno / r4 / pico / esp32 / unoq × their typical modules), then consolidate Tier 0
alongside. Tier 2/3 come after, once there's a green baseline worth protecting — and they realize
roadmap #1's build-matrix line as a local runner rather than CI.

## What landed (2026-06-16)

All four tiers, built in the order above.

- **Tier 0** — `tests/run.sh` is the single consolidated host gate. It compiles+runs the existing
  channel-codec / NEC-Sony tests *plus* new coverage: `core/tests/test_registry.cpp`
  (dispatch/Writer/SystemModule/overflow/duplicate-id panic), `modules/locomotion/tests/test_drivemixer.cpp`
  (two-zone curve, ramping, spin + LocoProtocol pack/unpack), `modules/controller/tests/test_calibration.cpp`
  (re-center/rescale/deadzone). Also runs the broker PTY-loopback and the new codec↔broker guard.
- **Tier 1** — `tools/cmdr/tests/` (pytest). 24 golden `commander_modules.h` snapshots under
  `golden/` over a (target × module-set) matrix; invariant lint (unique includes, no designated
  initializers, balanced braces, system-first, register-line dedup, the unoq channel-bus-hook vs
  UART-hook split); honest-menu gating; manifest round-trip; ESP32 partition composition; and
  `cmd_init` / `cmd_module` scaffolding + enable/disable idempotence with subprocess+`input` stubbed.
  Update goldens with `CMDR_UPDATE_GOLDEN=1`.
- **codec↔broker byte-compat guard** — `transport/channels/tests/codec_harness.cpp` exposes the real
  C codec as a Unix filter; `test_codec_compat.py` round-trips tricky frames (embedded zeros, >254
  run, multi-frame) through it and the broker's Python port, **both directions**, byte-for-byte.
  This is the permanent home for what was a manual cross-check — it caught nothing yet because the
  two are in sync, which is the point.
- **Tier 2/3** — `tests/build-matrix.sh`. Toolchain-detecting, skip-with-notice. It `cmdr init`s a
  throwaway project per row, points it at *this* checkout (`cmdr link` for CMake incl. unoq — its
  `FetchContent_Populate` gets the same local hook injected — and a `symlink://` lib_dep for
  PlatformIO, so it compiles the working tree, not GitHub `main`), enables a representative module
  set, and runs the real generated build script. `--full` is Tier 3 (every config); `--list` shows
  the matrix. Build dirs are reused under `$CMDR_MATRIX_CACHE`. **Validated 2026-06-16: all six
  platforms compile the local tree here — pico/base, pico/sensors, r4/wifi_ir, uno/ir, bluepill/base,
  esp32/base, unoq/base all PASS** (esp32 after sourcing the IDF env, unoq via the Zephyr venv + gnuarmemb).

Notes / gotchas found while building it:
- Project names must avoid the Pico SDK's reserved `pico_*` target namespace (a project literally
  named `pico_base` breaks `pico_add_extra_outputs`); the matrix prefixes every throwaway with `mtx_`.
- `FETCHCONTENT_SOURCE_DIR_COMMANDER` as an *environment* variable is **not** honored by CMake's
  FetchContent — only the CMake cache var is. So local-tree builds go through `cmdr link` (which
  writes that cache var into `commander_local.cmake`), not an env export. Pico/Pico 2 W `init` still
  does one quick GitHub fetch during its auto-configure before `cmdr link` swaps in the local source.
- ESP-IDF builds are `-Werror` + misleading-indentation; the broadened Tier 0 tests are host-g++ so
  they don't see that, but the build matrix (Tier 2) does — it's the tier that catches it.
- ESP-IDF and Zephyr aren't on the bare PATH — they're activated by an env script (the `esp` alias
  sources `export.sh`; the unoq `build` script sources the Zephyr venv at `~/zephyrproject/.venv`).
  So `build-matrix.sh` probes for those *activators* (`idf_export` / `zephyr_venv`, overridable via
  `$IDF_EXPORT` / `$ZEPHYR_VENV`) and activates ESP-IDF itself before building, rather than checking
  for `idf.py`/`west` on PATH. The Uno Q **compiles** locally (gnuarmemb, since the Zephyr SDK has no
  Intel-Mac build); only *flashing* it needs an adb-reachable board.
