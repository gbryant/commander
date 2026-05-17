#include "BootselModule.h"
#include "core/CommandRegistry.h"
#include "i2c_ids.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

#define DIRECT_MAGIC 0xb007b007u

void BootselModule::checkAtBoot() {
    if (watchdog_hw->scratch[7] == DIRECT_MAGIC) {
        watchdog_hw->scratch[7] = 0;
        reset_usb_boot(0, 0);
        for (;;) {}
    }
    watchdog_hw->scratch[7] = 0;
}

static void resetHandler(const char *, Writer &out, void *) {
    out.writeln("Rebooting...");
    watchdog_reboot(0, 0, 500);
}

static void bootloaderHandler(const char *, Writer &out, void *) {
    out.writeln("Entering USB bootloader...");
    watchdog_hw->scratch[7] = DIRECT_MAGIC;
    watchdog_reboot(0, 0, 500);
}

void BootselModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("reset",      "reboot the firmware",  CMD_RESET,      resetHandler,      this));
    reg.registerCommand(CMD("bootloader", "enter USB bootloader", CMD_BOOTLOADER, bootloaderHandler, this));
}
