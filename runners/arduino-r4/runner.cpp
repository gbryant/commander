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

void setup() {
    commander_early_init();
    _cfg = commander_config();

    const char *greeting = _cfg.uart_greeting ? _cfg.uart_greeting : "commander";
    _uart.begin(_registry, _cfg.uart_baud, greeting);

    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    commander_setup(_registry);
    _registry.validateIds();

    // R4 USB CDC: write() and read() both return 0/-1 until the terminal
    // asserts DTR. Wait here (pre-scheduler, busy-wait) so the greeting and
    // prompt land correctly. Timeout after 10 s for headless / OTA use.
    for (uint32_t t = millis(); !Serial && (millis() - t) < 10000; ) delay(10);

    commander_on_uart_ready(_uart);
    xTaskCreate(UartTransport::taskBody, "uart", 256, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid) {
        Serial.print("[wifi] connecting");
        WiFi.begin(_cfg.wifi_ssid, _cfg.wifi_password);
        for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
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
    }

    vTaskStartScheduler();
}

void loop() {}

#endif // COMMANDER_R4_RUNNER
