#include "commander.h"
#include "BootselModule.h"
#ifdef COMMANDER_ENABLE_OTA
#include "ota_cmd.h"
#endif
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal/hal.h"
#include "transport/telnet/TelnetTransport.h"
#include <stdio.h>

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;
static TelnetTransport _telnet;
static BootselModule   _bootsel;

// ── FreeRTOS panic hooks ──────────────────────────────────────────────────
// These run from the tick ISR — printf is unsafe (may deadlock on the stdio
// spinlock). Use watchdog scratch[6] to pass the panic type across the reset
// boundary; main() reads and prints it after USB CDC is up.
#define PANIC_MAGIC_MALLOC 0x0BAD0001u
#define PANIC_MAGIC_STACK  0x0BAD0002u

extern "C" {
    void vApplicationMallocFailedHook(void) {
        watchdog_hw->scratch[6] = PANIC_MAGIC_MALLOC;
        watchdog_reboot(0, 0, 0);
        for (;;) {}
    }
    void vApplicationStackOverflowHook(TaskHandle_t, char *) {
        watchdog_hw->scratch[6] = PANIC_MAGIC_STACK;
        watchdog_reboot(0, 0, 0);
        for (;;) {}
    }
    void vApplicationIdleHook(void) {}
}

// ── Weak hook defaults ────────────────────────────────────────────────────
// Apps override any of these by defining the same symbol without __weak__.
extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

// ── Main FreeRTOS task ────────────────────────────────────────────────────
static void runnerTask(void *) {
    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    _registry.registerModule(_bootsel);
    commander_setup(_registry);
#ifdef COMMANDER_ENABLE_OTA
    pfb_firmware_commit();
    _registry.registerCommand(CMD("ota", "flash firmware from URL (http)", I2C_NONE, cmdOta, nullptr));
#endif
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    xTaskCreate(UartTransport::taskBody, "uart", 1024, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid) {
        printf("[wifi] ssid='%s' connecting...\n", _cfg.wifi_ssid);
        if (cyw43_arch_init() == 0) {
            cyw43_arch_enable_sta_mode();
            int err = -1;
            for (int i = 0; i < 3 && err != 0; i++) {
                if (i > 0) { printf("[wifi] retry %d...\n", i); vTaskDelay(pdMS_TO_TICKS(5000)); }
                err = cyw43_arch_wifi_connect_timeout_ms(
                    _cfg.wifi_ssid, _cfg.wifi_password,
                    CYW43_AUTH_WPA2_MIXED_PSK, 15000);
                printf("[wifi] connect=%d\n", err);
            }
            if (err == 0) {
                printf("[wifi] mdns init\n");
                cyw43_arch_lwip_begin();
                mdns_resp_init();
                mdns_resp_add_netif(netif_default, _cfg.hostname);
                cyw43_arch_lwip_end();
                printf("[wifi] mdns ok\n");

                if (_cfg.enable_telnet) {
                    printf("[wifi] telnet start\n");
                    const char *tgreeting = _cfg.telnet_greeting
                                          ? _cfg.telnet_greeting
                                          : _cfg.hostname;
                    _telnet.begin(_registry, tgreeting);
                    xTaskCreate(TelnetTransport::taskBody, "telnet", 4096, &_telnet, 2, nullptr);
                    printf("[wifi] telnet ok\n");
                }
                commander_on_wifi_connected();
                printf("[wifi] ready\n");
            } else {
                printf("[wifi] connect failed (%d)\n", err);
            }
        }
    }

    vTaskDelete(nullptr);
}

// ── Entry point ───────────────────────────────────────────────────────────
int main() {
    BootselModule::checkAtBoot();
    commander_early_init();
    _cfg = commander_config();

    stdio_init_all();
    sleep_ms(1000);  // let USB CDC enumerate

    uint32_t panic_code = watchdog_hw->scratch[6];
    watchdog_hw->scratch[6] = 0;
    if (panic_code == PANIC_MAGIC_MALLOC) printf("[PANIC] malloc failed — rebooted\n");
    if (panic_code == PANIC_MAGIC_STACK)  printf("[PANIC] stack overflow — rebooted\n");

    _uart.begin(_registry, _cfg.uart_baud, _cfg.uart_greeting);

    xTaskCreate(runnerTask, "main", 8192, nullptr, 1, nullptr);
    vTaskStartScheduler();
    for (;;) {}
}
