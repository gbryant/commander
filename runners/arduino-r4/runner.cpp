#ifdef COMMANDER_R4_RUNNER

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <WiFiS3.h>
#include "commander.h"
#include "hal/hal.h"
// Unity-build: only compiled for R4, keeps it out of library srcFilter
#include "transport/telnet/arduino/ArduinoTelnetTransport.cpp"

static CommanderConfig        _cfg;
static CommandRegistry        _registry;
static UartTransport          _uart;
static ArduinoTelnetTransport _telnet;
static WiFiServer             _server(23);

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

static void wifiTask(void *) {
    // Match the UART task's 1500ms settle — USB CDC must be ready before first print.
    // WiFi SPI is also slow on R4 (~5x slower than R3); running in a task lets the
    // UART task and USB interrupt handler stay live during the connection loop.
    vTaskDelay(pdMS_TO_TICKS(1500));
    Serial.print("[wifi] connecting");
    WiFi.begin(_cfg.wifi_ssid, _cfg.wifi_password);
    for (int i = 0; i < 40 && (WiFi.status() != WL_CONNECTED ||
                                WiFi.localIP() == IPAddress(0, 0, 0, 0)); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.print("[wifi] ");
        Serial.println(WiFi.localIP());
        if (_cfg.enable_telnet) {
            const char *tg = _cfg.telnet_greeting ? _cfg.telnet_greeting : _cfg.hostname;
            _server.begin();
            _telnet.begin(_registry, _server, tg);
            xTaskCreate(ArduinoTelnetTransport::taskBody, "telnet", 256, &_telnet, 2, nullptr);
        }
        commander_on_wifi_connected();
    } else {
        Serial.println("[wifi] failed — telnet disabled");
    }
    vTaskDelete(nullptr);
}

void setup() {
    commander_early_init();
    _cfg = commander_config();

    const char *greeting = _cfg.uart_greeting ? _cfg.uart_greeting : "commander";
    _uart.begin(_registry, _cfg.uart_baud, greeting);

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    xTaskCreate(UartTransport::taskBody, "uart", 256, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid)
        xTaskCreate(wifiTask, "wifi", 512, nullptr, 1, nullptr);

    vTaskStartScheduler();
}

void loop() {}

#endif // COMMANDER_R4_RUNNER
