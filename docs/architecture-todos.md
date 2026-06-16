# Architecture Review — Action Items

Generated from a codebase review on 2026-06-16. Grouped by priority.

---

## Must (before public release)

- [ ] **CI matrix** — add `.github/workflows/` that compiles all targets on every
      push: `cmake` for Pico/Pico2/ESP32, `pio run` for Uno/R4/Bluepill. Without
      this, contributors will silently break platforms they can't test locally.

- [ ] **`init()` → `bool`** — `IModule::init()` currently returns void, so a missing
      sensor (wrong address, not wired) registers its commands anyway and they fail
      silently. Change the return type to bool; have `CommandRegistry::registerModule()`
      skip registration and print a warning when it returns false.

---

## Should

- [ ] **Ticker support in TelnetTransport** — `UartTransport::addTicker()` lets
      modules (e.g. IR) pump their state machine each poll cycle. TelnetTransport has
      no equivalent, so those modules don't work over telnet. Either add the same
      `addTicker()` API to TelnetTransport or document the constraint explicitly.

- [ ] **I2C slave in HAL** — the R4 locomotion bridge (`runners/arduino-r4/`) uses
      Arduino `Wire.onReceive/onRequest` directly, bypassing `hal.h` entirely. Any
      future slave platform needs its own one-off impl. Add `hal_i2c_slave_init()` /
      `hal_i2c_slave_on_receive()` / `hal_i2c_slave_on_request()` to keep portability
      consistent with the rest of the HAL.

- [ ] **PORTING.md** — document how to bring up a new target: implement
      `hal/<platform>/hal.cpp`, write a `runners/<platform>/runner.cpp`, wire the
      build system. Currently this knowledge lives in CLAUDE.md fragments and
      existing platform code.

- [ ] **MAX_COMMANDS overflow is runtime-only** — on Uno (`-DMAX_COMMANDS=12`) adding
      one more sensor quietly drops a command at boot. Add a `cmdr` pre-flight check
      (or a CMake/platformio static assert) that counts registered commands against
      the cap before flashing.

---

## Nice to Have

- [ ] **Unified debug/log macro** — platform code uses raw `printf()`, modules use
      `out.writeln()`. Add a `CMDR_LOG(level, msg)` macro that can be routed to UART
      or suppressed in `LOW_MEMORY_MODE`.

- [ ] **Split `LOW_MEMORY_MODE`** — the current flag strips both command names and help
      text, and switches to byte-based dispatch (making the shell nearly unusable
      interactively). Consider an intermediate mode that keeps names but drops help,
      or make them separate flags (`LOW_MEMORY_HELP` / `LOW_MEMORY_DISPATCH`).

- [ ] **WiFi logic consolidation** — `WifiModule.h` is the command interface but the
      link watchdog, reconnect, and mDNS re-announce live in the runner. Adding new
      WiFi features requires touching both. Consider a `WifiWatchdogModule` that owns
      the link-watching loop and emits the existing weak hooks.

- [ ] **Architecture diagram** — PLAN.md has a good ASCII diagram of the layers but
      nothing showing how the runner, transport, modules, and HAL interact at runtime
      (task model, call flow). A simple diagram would help first-time contributors.
