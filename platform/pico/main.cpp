#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2cModule.h"
#include "modules/SonarModule.h"
#include "transport/uart/UartTransport.h"
#include "BootselModule.h"

static CommandRegistry registry;
static SystemModule    systemModule;
static CompassModule   compassModule;
static I2cModule       i2cModule;
static SonarModule     sonarModule(6);  // Grove GP6
static BootselModule   bootselModule;
static UartTransport   uart;

extern "C" {
    void vApplicationMallocFailedHook(void) {
        printf("[PANIC] malloc failed\n"); for (;;) {}
    }
    void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
        printf("[PANIC] stack overflow: %s\n", name); for (;;) {}
    }
}

static void mainTask(void *) {
    hal_i2c_init(4, 5, 400000);  // SDA=GP4, SCL=GP5

    registry.registerModule(systemModule);
    registry.registerModule(compassModule);
    registry.registerModule(i2cModule);
    registry.registerModule(sonarModule);
    registry.registerModule(bootselModule);
    registry.validateIds();

    // UartTransport task — 1 KB stack is comfortable on Pico
    xTaskCreate(UartTransport::taskBody, "uart", 1024, &uart, 2, nullptr);

    vTaskDelete(nullptr);
}

int main() {
    BootselModule::checkAtBoot();
    stdio_init_all();
    sleep_ms(1000);  // let USB CDC enumerate

    uart.begin(registry, 115200, "commander/pico");

    xTaskCreate(mainTask, "main", 2048, nullptr, 1, nullptr);
    vTaskStartScheduler();
    for (;;) {}
}
