# Design note: a test suite + local build-test system

**Status:** design note only — NOT started. Captured 2026-06-15. This is the detailed plan behind
roadmap #1 ("Tests + CI for `cmdr` and a platform build matrix"). [[project_roadmap]]

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

### Tier 3 — full build matrix in CI

Tier 2 exhaustively: all boards × representative module combinations, in containers (PlatformIO and
ESP-IDF ship Docker images; Pico SDK and Zephyr both build in Linux CI — Zephyr won't build on the
Intel Mac, but **does** build in a Linux CI container). Optionally matrix-build the example consumer
repos (`cmdr-robot`, `cmdr-oi-bridge`, …) so their adoption path is guarded too.

## The local build-test system (mechanics)

The constraint that shapes everything: **you can't build everything on the Mac.** Zephyr/Uno Q
builds happen on the SBC (the Zephyr SDK dropped Intel-Mac support; a gnuarmemb chain builds
on-board), and ESP-IDF needs `esp` sourced first. So the local runner must be
**toolchain-detecting and skip-with-notice**, never all-or-nothing:

- **`tests/run.sh`** → Tier 0 + Tier 1 only. No toolchains, runs in seconds. **This is the
  pre-commit gate** — everything that doesn't need a cross-compiler.
- **`tests/build-matrix.sh`** → Tier 2. Probes what's available (`pio`; `cmake` + `PICO_SDK_PATH`;
  `idf.py` after `esp`; an ssh/adb-reachable board for Zephyr), builds the configs it can, and
  prints `SKIP unoq (board unreachable)` for the rest. **Pre-push / on-demand**, minutes. Reuses
  per-board build dirs for speed.
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
alongside. Tier 2/3 come after, once there's a green baseline worth protecting — and they slot
straight into roadmap #1's build-matrix CI line.
