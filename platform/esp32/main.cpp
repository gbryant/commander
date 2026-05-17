#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/SonarModule.h"
#include "transport/uart/UartTransport.h"

static const char *TAG = "commander";

static CommandRegistry registry;
static SystemModule    systemModule;
static CompassModule   compassModule;
static SonarModule     sonarModule(4);  // set GPIO for your wiring
static UartTransport   uart;

static void mainTask(void *) {
    hal_i2c_init(21, 22, 400000);  // SDA=GPIO21, SCL=GPIO22

    registry.registerModule(systemModule);
    registry.registerModule(compassModule);
    registry.registerModule(sonarModule);
    registry.validateIds();

    xTaskCreate(UartTransport::taskBody, "uart", 2048, &uart, 2, nullptr);

    ESP_LOGI(TAG, "commander/esp32");
    vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
    uart.begin(registry, 115200);
    xTaskCreate(mainTask, "main", 4096, nullptr, 1, nullptr);
}
