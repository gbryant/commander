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
#include "PicoIRModule.h"
#include "pico_fota_bootloader/core.h"
#include "ota_cmd.h"

static CommandRegistry registry;
static SystemModule    systemModule;
static CompassModule   compassModule;
static I2cModule       i2cModule;
static SonarModule     sonarModule(6);  // Grove GP6
static BootselModule   bootselModule;
static PicoIRModule    irModule(22);  // Grove IR Receiver v1.2 on GP22
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
    void vApplicationIdleHook(void) {}
}

static void mainTask(void *) {
    pfb_firmware_commit();  // firmware is running — prevent rollback on next reboot

    hal_i2c_init(4, 5, 100000);  // SDA=GP4, SCL=GP5

    registry.registerModule(systemModule);
    registry.registerModule(compassModule);
    registry.registerModule(i2cModule);
    registry.registerModule(sonarModule);
    registry.registerModule(bootselModule);
    registry.registerModule(irModule);
    registry.registerCommand(CMD("ota", "flash firmware from URL (http)", I2C_NONE, cmdOta, nullptr));
    registry.validateIds();

    uart.addTicker(irModule);
    xTaskCreate(UartTransport::taskBody, "uart", 1024, &uart, 2, nullptr);

    // cyw43_arch_init must be called from a task (after scheduler) with lwip_freertos
    printf("[wifi] ssid='%s' connecting...\n", WIFI_SSID);
    if (cyw43_arch_init() == 0) {
        cyw43_arch_enable_sta_mode();

        int err = -1;
        for (int attempt = 0; attempt < 3 && err != 0; attempt++) {
            if (attempt > 0) {
                printf("[wifi] retry %d...\n", attempt);
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            err = cyw43_arch_wifi_connect_timeout_ms(
                WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_MIXED_PSK, 15000);
            printf("[wifi] connect=%d\n", err);
        }
        if (err == 0) {
#ifdef PICO_RP2350
            static const char* hostname = "pico2";
            static const char* greeting = "commander/pico2";
#else
            static const char* hostname = "pico";
            static const char* greeting = "commander/pico";
#endif
            cyw43_arch_lwip_begin();
            mdns_resp_init();
            mdns_resp_add_netif(netif_default, hostname);
            cyw43_arch_lwip_end();

            telnet.begin(registry, greeting);
            xTaskCreate(TelnetTransport::taskBody, "telnet", 4096, &telnet, 2, nullptr);
        } else {
            printf("[wifi] connect failed (%d)\n", err);
        }
    }

    irModule.launch();  // enable PIO + start core1 after WiFi
    printf("ir ready (GP%d)\r\n", 22);
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

#ifdef PICO_RP2350
    uart.begin(registry, 115200, "commander/pico2");
#else
    uart.begin(registry, 115200, "commander/pico");
#endif

    xTaskCreate(mainTask, "main", 8192, nullptr, 1, nullptr);
    vTaskStartScheduler();
    for (;;) {}
}
