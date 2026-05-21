#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mdns.h"
#include <string.h>

#include "secrets.h"
#include "hal/hal.h"
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/I2cModule.h"
#include "modules/Ina219Module.h"
#include "transport/uart/UartTransport.h"
#include "transport/telnet/TelnetTransport.h"
#include "ota_cmd.h"

static const char *TAG = "commander";

static CommandRegistry registry;
static SystemModule    systemModule;
static I2cModule       i2cModule;
static Ina219Module    ina_a(0x40, "a");
static Ina219Module    ina_b(0x45, "b");
static UartTransport   uart;
static TelnetTransport telnet;

static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi(void *, esp_event_base_t, int32_t id, void *) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
}

static void on_ip(void *, esp_event_base_t, int32_t id, void *) {
    if (id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
}

static bool wifi_connect() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    s_wifi_eg = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    on_wifi, nullptr);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_ip,   nullptr);

    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid,     WIFI_SSID,     sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void mainTask(void *) {
    hal_i2c_init(8, 9, 50000);  // SDA=GPIO8, SCL=GPIO9

    registry.registerModule(systemModule);
    registry.registerModule(i2cModule);
    registry.registerModule(ina_a);
    registry.registerModule(ina_b);
    registry.registerCommand(CMD("ota", "flash firmware from URL (http)", I2C_NONE, cmdOta, nullptr));
    registry.validateIds();

    xTaskCreate(UartTransport::taskBody, "uart", 4096, &uart, 2, nullptr);

    if (wifi_connect()) {
        mdns_init();
        mdns_hostname_set("esp32");
        telnet.begin(registry, "commander/esp32s3");
        xTaskCreate(TelnetTransport::taskBody, "telnet", 6144, &telnet, 2, nullptr);
    } else {
        ESP_LOGW(TAG, "wifi connect failed — telnet disabled");
    }

    ESP_LOGI(TAG, "commander ready");
    vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
    uart.begin(registry, 115200, "commander/esp32s3");
    xTaskCreate(mainTask, "main", 4096, nullptr, 1, nullptr);
}
