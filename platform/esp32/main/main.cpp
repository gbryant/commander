#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hal/hal.h"
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "transport/uart/UartTransport.h"

static const char *TAG = "commander";

static CommandRegistry registry;
static SystemModule    systemModule;
static UartTransport   uart;

static void mainTask(void *) {
    hal_i2c_init(21, 22, 400000);  // SDA=GPIO21, SCL=GPIO22

    registry.registerModule(systemModule);
    registry.validateIds();

    xTaskCreate(UartTransport::taskBody, "uart", 4096, &uart, 2, nullptr);

    ESP_LOGI(TAG, "commander ready");
    vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
    uart.begin(registry, 115200, "commander/esp32s3");
    xTaskCreate(mainTask, "main", 4096, nullptr, 1, nullptr);
}
