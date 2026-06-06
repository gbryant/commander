#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2CDiagModule.h"
#include "modules/SonarModule.h"
#include "transport/uart/UartTransport.h"
#include "IRModule.h"

static CommandRegistry registry;
static SystemModule    systemModule;
static CompassModule   compassModule;
static I2CDiagModule   i2cModule;
static SonarModule     sonarModule(6);  // Grove D6
static IRModule        irModule;
static UartTransport   uart;

static StackType_t  xUartStack[192];
static StaticTask_t xUartTCB;

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
    registry.registerModule(irModule);
    registry.validateIds();

    uart.addTicker(irModule);
    xTaskCreateStatic(UartTransport::taskBody, "uart", 192, &uart, 2, xUartStack, &xUartTCB);

    Serial.println(F("commander/arduino"));
}

void loop() {}
