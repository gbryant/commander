#include "commander.h"
#ifdef COMMANDER_ENABLE_OTA
#include "ota_cmd.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mdns.h"
#include "hal/hal.h"
#include "core/WifiHooks.h"
#include "transport/uart/UartTransport.h"
#include "transport/telnet/TelnetTransport.h"
#include <string.h>

static const char *TAG = "commander";

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;
static TelnetTransport _telnet;

// ── FreeRTOS panic hooks ──────────────────────────────────────────────────────
// These may run from ISR context. esp_rom_printf is ISR-safe; esp_restart() is
// safe to call from any context on ESP32. RTC_NOINIT_ATTR survives esp_restart(),
// so the panic type is logged after the reboot when the console is up.
#define PANIC_MAGIC_MALLOC 0x0BAD0001u
#define PANIC_MAGIC_STACK  0x0BAD0002u
#define PANIC_MAGIC_CMDID  0x0BAD0003u
RTC_NOINIT_ATTR static uint32_t s_panic_code;

extern "C" void vApplicationMallocFailedHook(void) {
    s_panic_code = PANIC_MAGIC_MALLOC;
    esp_restart();
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *) {
    s_panic_code = PANIC_MAGIC_STACK;
    esp_restart();
}

void commander_on_panic() {
    s_panic_code = PANIC_MAGIC_CMDID;
    esp_restart();
}

// ── Weak hook defaults ────────────────────────────────────────────────────────
extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0

static int            s_wifi_retries = 0;
static esp_netif_t   *_sta_netif     = nullptr;  // for IP lookup in the wifi command
static bool           _wifi_suppress = false;    // `wifi off` suppresses auto-reconnect
static bool           _wifi_services_up = false;

// Bring up mDNS + telnet on the first successful connect; re-call the app hook
// on every subsequent reconnect (mDNS re-announces automatically via IDF internals).
// Idempotent — safe to call from on_ip() in the event loop task context.
static void wifiBringUpServices() {
    if (!_wifi_services_up) {
        _wifi_services_up = true;
        mdns_init();
        mdns_hostname_set(_cfg.hostname);
        if (_cfg.enable_telnet) {
            const char *tg = _cfg.telnet_greeting ? _cfg.telnet_greeting : _cfg.hostname;
            _telnet.begin(_registry, tg);
            xTaskCreate(TelnetTransport::taskBody, "telnet", 6144, &_telnet, 2, nullptr);
        }
    }
    commander_on_wifi_connected();
}

static void on_wifi(void *, esp_event_base_t, int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "disconnected reason: %d (attempt %d)", (int)d->reason, s_wifi_retries);
        if (_wifi_suppress) return;   // user ran `wifi off` — stay down
        s_wifi_retries++;
        esp_wifi_connect();           // always retry, indefinitely (mirrors Pico wifiMonitor)
    }
}

static void on_ip(void *, esp_event_base_t, int32_t id, void *) {
    if (id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        wifiBringUpServices();  // idempotent — first call starts services, later ones just hook
    }
}

static bool wifi_connect(const char *ssid, const char *password) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    _sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    s_wifi_eg = xEventGroupCreate();
    esp_event_handler_instance_t inst_any_id;
    esp_event_handler_instance_t inst_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    on_wifi, nullptr, &inst_any_id);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_ip,   nullptr, &inst_got_ip);

    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid,     ssid,     sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable    = true;
    wc.sta.pmf_cfg.required   = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_set_max_tx_power(20);
    // esp_wifi_connect() is called from the WIFI_EVENT_STA_START handler.
    // Wait up to 30 s for the initial connect; if it times out the event handler
    // keeps retrying in the background and on_ip() brings up services whenever
    // the link comes up — same pattern as the Pico's wifiMonitor().
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                            WIFI_CONNECTED_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// ── WiFi command hooks (modules/WifiModule.h → `wifi status|off|on`) ────────────
extern "C" bool commander_wifi_status(WifiInfo *info) {
    memset(info, 0, sizeof(*info));
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;  // not associated
    info->connected = true;
    strlcpy(info->ssid, (const char *)ap.ssid, sizeof(info->ssid));
    info->rssi = ap.rssi;
    if (_sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(_sta_netif, &ip) == ESP_OK)
            esp_ip4addr_ntoa(&ip.ip, info->ip, sizeof(info->ip));
    }
    return true;
}

extern "C" void commander_wifi_off() {
    _wifi_suppress = true;       // keep the disconnect handler from reconnecting
    esp_wifi_disconnect();
}

extern "C" void commander_wifi_on() {
    _wifi_suppress = false;
    s_wifi_retries = 0;
    esp_wifi_connect();
}

// ── Main FreeRTOS task ────────────────────────────────────────────────────────
static void runnerTask(void *) {
    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
    _registry.registerCommand(CMD("reset", "reboot the firmware", CMD_RESET,
        [](const char *, Writer &out, void *) {
            out.writeln("Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }, nullptr));
#ifdef COMMANDER_ENABLE_OTA
    _registry.registerCommand(CMD("ota", "flash firmware from URL (http)", I2C_NONE, cmdOta, nullptr));
#endif
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    commander_run_autostart(_registry);   // boot commands (cmdr autostart), e.g. `ir recv`
    xTaskCreate(UartTransport::taskBody, "uart", 4096, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid) {
        ESP_LOGI(TAG, "connecting to '%s'...", _cfg.wifi_ssid);
        if (!wifi_connect(_cfg.wifi_ssid, _cfg.wifi_password)) {
            // Not up within 30 s — event handler keeps retrying in the background;
            // on_ip() will start mDNS + telnet when the link eventually comes up.
            ESP_LOGW(TAG, "wifi not up yet — telnet pending background connect");
        }
        // Services are started by on_ip() whenever connection succeeds (now or later).
    }

    ESP_LOGI(TAG, "commander ready");
    vTaskDelete(nullptr);
}

// ── Entry point ───────────────────────────────────────────────────────────────
extern "C" void app_main(void) {
    if (s_panic_code == PANIC_MAGIC_MALLOC) ESP_LOGW(TAG, "[PANIC] malloc failed — rebooted");
    if (s_panic_code == PANIC_MAGIC_STACK)  ESP_LOGW(TAG, "[PANIC] stack overflow — rebooted");
    if (s_panic_code == PANIC_MAGIC_CMDID)  ESP_LOGW(TAG, "[PANIC] duplicate command ID — rebooted");
    s_panic_code = 0;

    commander_early_init();
    _cfg = commander_config();

    _uart.begin(_registry, _cfg.uart_baud, _cfg.uart_greeting);
    xTaskCreate(runnerTask, "main", 4096, nullptr, 1, nullptr);
}
