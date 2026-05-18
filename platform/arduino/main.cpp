#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2cModule.h"
#include "modules/SonarModule.h"
#include "transport/uart/UartTransport.h"

static CommandRegistry registry;
static SystemModule    systemModule;
static CompassModule   compassModule;
static I2cModule       i2cModule;
static SonarModule     sonarModule(4);  // Grove D4
static UartTransport   uart;

extern "C" void vApplicationMallocFailedHook() {
    Serial.print(F("[PANIC] malloc failed\r\n"));
    for (;;) {}
}

void setup() {
    uart.begin(registry, 115200);      // inits Serial before scheduler

    hal_i2c_init(SDA, SCL, 400000);

    registry.registerModule(systemModule);
    registry.registerModule(compassModule);
    registry.registerModule(i2cModule);
    registry.registerModule(sonarModule);
    registry.validateIds();

    xTaskCreate(UartTransport::taskBody, "uart", 192, &uart, 2, nullptr);

    Serial.println(F("commander/arduino"));
}

void loop() {}
