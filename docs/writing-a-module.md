# Writing your own module

The framework's promise is that a module is written once, against a small C HAL,
and runs on every target. This guide shows the module API, the rules that keep a
module portable, and how to wire one into your app. For the stock modules and
what they look like from the shell, see [modules.md](modules.md).

## The model

A module is a class implementing `core/IModule.h`:

```cpp
class IModule {
public:
    virtual const char *name()                           const = 0;
    virtual void        init()                                 = 0;  // bring up hardware
    virtual void        registerCommands(CommandRegistry &reg) = 0;  // add shell commands
    virtual void        startTask()                            {}   // optional: spawn a task
    virtual void        tick()                                 {}   // optional: pumped I/O
};
```

`CommandRegistry::registerModule()` calls `init()` then `registerCommands()`.
Commands are static `Command` entries — name, help line, an I2C command ID, a
handler, and a context pointer (usually `this`).

**Use `I2C_NONE` for the ID** unless the command is genuinely part of the wire
protocol in `include/i2c_ids.h`. `I2C_NONE` (`0xFF`) means "text-only, not
dispatched over I2C", and it's the only value `validateIds()` allows to repeat —
every stock module uses it. Do **not** pass `0`: that's a real ID
(`CMD_HELP`, which `SystemModule` always registers), so a second command
claiming it collides and panics the board at boot with
`[PANIC] duplicate command id: 0x00`.

## A minimal module

A complete module that reads a GPIO-connected tilt switch:

```cpp
#pragma once
#include "core/CommandRegistry.h"
#include "hal/hal.h"

class TiltModule : public IModule {
public:
    explicit TiltModule(int pin) : _pin(pin) {}

    const char *name() const override { return "tilt"; }

    void init() override {
        hal_gpio_set_input(_pin);
    }

    void registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD(
            "tilt", "tilt switch state", I2C_NONE,
            [](const char *, Writer &out, void *ctx) {
                auto *self = static_cast<TiltModule *>(ctx);
                out.writeln(hal_gpio_read(self->_pin) ? "TILTED" : "level");
            },
            this));
    }

private:
    int _pin;
};
```

Drop the header next to your `main.cpp` and register it in `commander_setup()`,
alongside whatever `cmdr` generated:

```cpp
#include "commander_modules.h"   // generated — stock modules
#include "TiltModule.h"

static TiltModule tilt(7);

extern "C" void commander_setup(CommandRegistry &reg) {
    commander_register_modules(reg);   // cmdr-managed modules
    reg.registerModule(tilt);          // yours
}
```

That's the whole loop: no cmdr involvement is needed for your own modules —
`cmdr` composes the *stock* set; `commander_setup()` is yours.

## The CMD macro gotcha

The C preprocessor sees commas inside `{}` lambda bodies as extra macro
arguments. Declare multiple variables on separate lines inside `CMD(...)`
handlers:

```cpp
// WRONG — breaks the CMD macro:
int16_t x, y, z;
// RIGHT:
int16_t x; int16_t y; int16_t z;
```

## Rules that keep a module portable

- **Include only `hal/hal.h` and `core/` headers.** The HAL is a C interface:
  I2C (`hal_i2c_init/probe/write/read/read_raw`), GPIO
  (`hal_gpio_set_output/set_input/write/read`, `hal_pulse_in_us`), time
  (`hal_delay_ms`, `hal_time_us`), and UART (`hal_uart_*`). If a module needs
  anything else, it isn't platform-independent — put the platform half behind
  an interface (see below).
- **Write output through the `Writer`,** never a platform print. `writeln()`
  for lines; check `out.ok()` in loops — it goes false when the client
  disconnects, and streaming commands should exit cleanly:

  ```cpp
  while (streaming && out.ok()) { /* emit */ }
  ```
- **Don't block.** Long waits stall the transport task. For continuous work,
  implement `tick()` and have the UART task pump it (below), or `startTask()`
  to own a FreeRTOS task.
- **Watch the command budget.** `MAX_COMMANDS` bounds the registry. `cmdr`
  sizes it for the modules *it* manages plus a small reserve — it cannot see
  commands your app registers, so if you add more than a handful, raise
  `MAX_COMMANDS` yourself in `CMakeLists.txt` / `platformio.ini`. `cmdr` will
  never lower a value you've raised, and the registry warns at boot and in
  `help` if anything was dropped (check `dropped()`).

## Getting `tick()` pumped

If your module polls hardware (like IR receive), add it as a ticker in the
`commander_on_uart_ready` hook — the UART task calls `tick()` between input:

```cpp
extern "C" void commander_on_uart_ready(UartTransport &uart) {
    uart.addTicker(myModule);
}
```

The other optional hooks (`commander.h`): `commander_early_init()`
(pre-scheduler), `commander_on_wifi_connected()` (post-WiFi), and
`commander_on_panic()` (a weak symbol in `core/CommandRegistry.cpp` — override
for board-specific crash diagnostics).

## When a module can't be fully portable

Split it: a neutral interface in `modules/`, one implementation per platform.
This is the IR pattern — `modules/ir/IIRModule.h` defines the vocabulary, and
`platform/pico/PicoIRModule.h` (PIO), `platform/arduino/IRModule.h` (IRremote),
`platform/esp32/Esp32IRModule.h` (RMT), etc. each implement it natively. The
`aicam` module does the same at a finer grain: the protocol client is portable,
and only the byte transport (`ISscmaTransport`) has platform backends.

Modules that publish data to apps use a **weak ready-hook**: the module header
declares `commander_on_<name>_ready(<Module>&)`, the generated file calls it,
and apps that care define it. Apps that don't pay nothing.

## Contributing a module to the stock set

To make a module composable via `cmdr module enable`, it needs an entry in
`MODULE_SPECS` (`tools/cmdr/src/cmdr/cli.py`): platforms, config questions with
per-target defaults, and an emitter fragment in `_emit_module()` that generates
its include/declaration/registration lines. Optional spec fields add build-flag
`features`, PlatformIO `pio_lib_deps`, companion host `tools` installed to the
project's `bin/`, and `seed_dirs`. Add a golden test in `tools/cmdr/tests/`
(the suite pins the generated `commander_modules.h` for every module — run
`tests/run.sh`).
