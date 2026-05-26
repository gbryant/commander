#include "commander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mdns.h"
#include "hal/hal.h"
#include "transport/uart/UartTransport.h"
#include "transport/telnet/TelnetTransport.h"
#include <string.h>

static const char *TAG = "commander";

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;
static TelnetTransport _telnet;

// ── Weak hook defaults ────────────────────────────────────────────────────────
extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi(void *, esp_event_base_t, int32_t id, void *) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
}

static void on_ip(void *, esp_event_base_t, int32_t id, void *) {
    if (id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
}

static bool wifi_connect(const char *ssid, const char *password) {
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
    strlcpy((char *)wc.sta.ssid,     ssid,     sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// ── Main FreeRTOS task ────────────────────────────────────────────────────────
static void runnerTask(void *) {
    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    xTaskCreate(UartTransport::taskBody, "uart", 4096, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid) {
        ESP_LOGI(TAG, "connecting to '%s'...", _cfg.wifi_ssid);
        if (wifi_connect(_cfg.wifi_ssid, _cfg.wifi_password)) {
            mdns_init();
            mdns_hostname_set(_cfg.hostname);
            if (_cfg.enable_telnet) {
                const char *tg = _cfg.telnet_greeting ? _cfg.telnet_greeting : _cfg.hostname;
                _telnet.begin(_registry, tg);
                xTaskCreate(TelnetTransport::taskBody, "telnet", 6144, &_telnet, 2, nullptr);
            }
            commander_on_wifi_connected();
        } else {
            ESP_LOGW(TAG, "wifi connect failed — telnet disabled");
        }
    }

    ESP_LOGI(TAG, "commander ready");
    vTaskDelete(nullptr);
}

// ── Entry point ───────────────────────────────────────────────────────────────
extern "C" void app_main(void) {
    commander_early_init();
    _cfg = commander_config();

    _uart.begin(_registry, _cfg.uart_baud, _cfg.uart_greeting);
    xTaskCreate(runnerTask, "main", 4096, nullptr, 1, nullptr);
}
