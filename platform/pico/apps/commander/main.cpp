#include "commander.h"
#include "secrets.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2cModule.h"
#include "modules/SonarModule.h"
#include "BootselModule.h"
#include "PicoIRModule.h"
#include "pico_fota_bootloader/core.h"
#include "ota_cmd.h"

static SystemModule  systemModule;
static CompassModule compassModule;
static I2cModule     i2cModule;
static SonarModule   sonarModule(6);        // Grove GP6
static BootselModule bootselModule;
static PicoIRModule  irModule(22);          // Grove IR Receiver v1.2 on GP22

extern "C" void commander_early_init() {
    BootselModule::checkAtBoot();
}

extern "C" CommanderConfig commander_config() {
    return {
        .wifi_ssid       = WIFI_SSID,
        .wifi_password   = WIFI_PASSWORD,
#ifdef PICO_RP2350
        .hostname        = "pico2",
#else
        .hostname        = "pico",
#endif
        .i2c_sda         = 4,
        .i2c_scl         = 5,
        .uart_baud       = 115200,
#ifdef PICO_RP2350
        .uart_greeting   = "commander/pico2",
        .telnet_greeting = "commander/pico2",
#else
        .uart_greeting   = "commander/pico",
        .telnet_greeting = "commander/pico",
#endif
    };
}

extern "C" void commander_setup(CommandRegistry &reg) {
    pfb_firmware_commit();  // firmware is running — prevent rollback on next reboot

    reg.registerModule(systemModule);
    reg.registerModule(compassModule);
    reg.registerModule(i2cModule);
    reg.registerModule(sonarModule);
    reg.registerModule(bootselModule);
    reg.registerModule(irModule);
    reg.registerCommand(CMD("ota", "flash firmware from URL (http)", I2C_NONE, cmdOta, nullptr));
}

extern "C" void commander_on_uart_ready(UartTransport &uart) {
    uart.addTicker(irModule);
}

extern "C" void commander_on_wifi_connected() {
    irModule.launch();  // enable PIO + start core1 after WiFi (avoids BADAUTH -7)
    printf("ir ready (GP%d)\r\n", 22);
}
