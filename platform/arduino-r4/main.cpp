#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <WiFiS3.h>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "transport/uart/UartTransport.h"
#include "transport/telnet/arduino/ArduinoTelnetTransport.h"
#include "secrets.h"

// ── Incremental bring-up for Arduino R4 WiFi ─────────────────────────────────
// Step 1: minimal FreeRTOS boot (confirmed)
// Step 2: uart transport + command shell (confirmed)
// Step 3: WiFi (confirmed)
// Step 4: telnet  ← current

static CommandRegistry        reg;
static SystemModule           sysmod;
static UartTransport          uart;
static ArduinoTelnetTransport telnet;
static WiFiServer             server(23);

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char* name) {
    Serial.print("[STACK OVERFLOW] ");
    Serial.println(name ? name : "?");
    for (;;) {}
}

static void blinkTask(void*) {
    pinMode(LED_BUILTIN, OUTPUT);
    for (;;) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void setup() {
    Serial.begin(115200);

    Serial.print("[wifi] connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 30 && (WiFi.status() != WL_CONNECTED ||
                                WiFi.localIP() == IPAddress(0, 0, 0, 0)); i++) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        Serial.print("[wifi] "); Serial.println(WiFi.localIP());
    } else {
        Serial.println("[wifi] failed");
    }

    sysmod.registerCommands(reg);
    uart.begin(reg, "commander r4");
    server.begin();
    telnet.begin(reg, server, "commander r4");

    if (xTaskCreate(blinkTask, "blink", 128, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("[err] blink"); return;
    }
    if (xTaskCreate(UartTransport::taskBody, "uart", 256, &uart, 2, nullptr) != pdPASS) {
        Serial.println("[err] uart"); return;
    }
    if (xTaskCreate(ArduinoTelnetTransport::taskBody, "telnet", 256, &telnet, 2, nullptr) != pdPASS) {
        Serial.println("[err] telnet"); return;
    }

    vTaskStartScheduler();
}

void loop() {}
