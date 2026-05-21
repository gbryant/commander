#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#include "secrets.h"
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2cModule.h"
#include "modules/SonarModule.h"
#include "transport/uart/UartTransport.h"
#include "transport/telnet/TelnetTransport.h"
#include "BootselModule.h"

static CommandRegistry registry;
static SystemModule    systemModule;
static CompassModule   compassModule;
static I2cModule       i2cModule;
static SonarModule     sonarModule(6);  // Grove GP6
static BootselModule   bootselModule;
static UartTransport   uart;
static TelnetTransport telnet;

// FreeRTOS panic hooks are called from the tick ISR — printf is unsafe there
// because it may block on the stdio spinlock held by a preempted task, causing
// a deadlock that freezes the serial session permanently.
// Use watchdog scratch[6] (survives reset; BSS does not) to pass the panic type
// across the reset boundary, then print it from main() after USB CDC is up.
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
}

static void mainTask(void *) {
    hal_i2c_init(4, 5, 100000);  // SDA=GP4, SCL=GP5

    registry.registerModule(systemModule);
    registry.registerModule(compassModule);
    registry.registerModule(i2cModule);
    registry.registerModule(sonarModule);
    registry.registerModule(bootselModule);
    registry.validateIds();

    xTaskCreate(UartTransport::taskBody, "uart", 1024, &uart, 2, nullptr);

    // cyw43_arch_init must be called from a task (after scheduler) with lwip_freertos
    if (cyw43_arch_init() == 0) {
        cyw43_arch_enable_sta_mode();
        int err = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 15000);
        if (err == 0) {
            cyw43_arch_lwip_begin();
            mdns_resp_init();
            mdns_resp_add_netif(netif_default, "pico");
            cyw43_arch_lwip_end();

            telnet.begin(registry, "commander/pico");
            xTaskCreate(TelnetTransport::taskBody, "telnet", 4096, &telnet, 2, nullptr);
        } else {
            printf("[wifi] connect failed (%d)\n", err);
        }
    }

    vTaskDelete(nullptr);
}

int main() {
    BootselModule::checkAtBoot();
    stdio_init_all();
    sleep_ms(1000);  // let USB CDC enumerate

    uint32_t panic_code = watchdog_hw->scratch[6];
    watchdog_hw->scratch[6] = 0;
    if (panic_code == PANIC_MAGIC_MALLOC) printf("[PANIC] malloc failed — rebooted\n");
    if (panic_code == PANIC_MAGIC_STACK)  printf("[PANIC] stack overflow — rebooted\n");

    uart.begin(registry, 115200, "commander/pico");

    xTaskCreate(mainTask, "main", 8192, nullptr, 1, nullptr);
    vTaskStartScheduler();
    for (;;) {}
}
