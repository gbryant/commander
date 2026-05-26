#include <Arduino.h>
#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoMDNS.h>
#include <Arduino_FreeRTOS.h>

#include "secrets.h"
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "modules/CompassModule.h"
#include "modules/I2cModule.h"
#include "transport/uart/UartTransport.h"
#include "transport/telnet/arduino/ArduinoTelnetTransport.h"

// Stack budget from 8 KB FreeRTOS heap (RA4M1):
//   idle   128w =  512 B  (created by vTaskStartScheduler)
//   uart   512w = 2048 B  (serial + command dispatch chain; 256w was too small)
//   ota    384w = 1536 B  (ArduinoOTA + mDNS via WiFiS3)
//   telnet 512w = 2048 B  (WiFiServer + WiFiClient)
//   mon    256w = 1024 B  (snprintf + Serial only)
//   TCBs   ~5 × 100 B =  500 B
//   Total  ~7668 B  →  ~524 B free  ← watch [mon] heap output
#define STACK_UART    512
#define STACK_OTA     384
#define STACK_TELNET  512
#define STACK_MON     256

static const char* HOSTNAME = "r4";

static CommandRegistry          registry;
static SystemModule             systemModule;
static CompassModule            compassModule;
static I2cModule                i2cModule;
static UartTransport            uart;
static WiFiServer               wifiServer(23);
static ArduinoTelnetTransport   telnet;
static WiFiUDP                  mdnsUdp;
static MDNS                     mdns(mdnsUdp);

static TaskHandle_t uartHandle   = nullptr;
static TaskHandle_t otaHandle    = nullptr;
static TaskHandle_t telnetHandle = nullptr;
static TaskHandle_t monHandle    = nullptr;

extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char* name) {
    Serial.print("[STACK OVERFLOW] ");
    Serial.println(name ? name : "?");
    for (;;) {}
}

static void otaTask(void*) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] lost, reconnecting...");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000)
                vTaskDelay(pdMS_TO_TICKS(1000));
            if (WiFi.status() == WL_CONNECTED) {
                Serial.print("[wifi] restored, IP=");
                Serial.println(WiFi.localIP());
                mdns.begin(WiFi.localIP(), HOSTNAME);
            }
        } else {
            ArduinoOTA.poll();
            mdns.run();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void monitorTask(void*) {
    for (;;) {
        char buf[80];
        snprintf(buf, sizeof(buf), "[mon] heap=%u  uart=%u ota=%u telnet=%u mon=%u",
            (unsigned)xPortGetFreeHeapSize(),
            (unsigned)uxTaskGetStackHighWaterMark(uartHandle),
            (unsigned)uxTaskGetStackHighWaterMark(otaHandle),
            (unsigned)uxTaskGetStackHighWaterMark(telnetHandle),
            (unsigned)uxTaskGetStackHighWaterMark(monHandle));
        Serial.println(buf);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void setup() {
    uart.begin(registry, 115200, "commander/r4");
    while (!Serial && millis() < 3000) {}

    hal_i2c_init(SDA, SCL, 400000);

    registry.registerModule(systemModule);
    registry.registerModule(compassModule);
    registry.registerModule(i2cModule);
    registry.validateIds();

    if (xTaskCreate(UartTransport::taskBody, "uart", STACK_UART, &uart, 2, &uartHandle) != pdPASS) {
        Serial.println("[err] uart task"); return;
    }
    if (xTaskCreate(monitorTask, "mon", STACK_MON, nullptr, 1, &monHandle) != pdPASS) {
        Serial.println("[err] mon task"); return;
    }

    Serial.print("[wifi] connecting to ");
    Serial.println(WIFI_SSID);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t t0 = millis();
    while ((WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0))
           && millis() - t0 < 30000) {
        yield();
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[wifi] connected, IP=");
        Serial.println(WiFi.localIP());

        ArduinoOTA.begin(WiFi.localIP(), HOSTNAME, "", InternalStorage);

        int mdnsOk = mdns.begin(WiFi.localIP(), HOSTNAME);
        Serial.print("[mdns] begin="); Serial.println(mdnsOk);

        if (xTaskCreate(otaTask, "ota", STACK_OTA, nullptr, 1, &otaHandle) != pdPASS) {
            Serial.println("[err] ota task"); return;
        }

        wifiServer.begin();
        telnet.begin(registry, wifiServer, "commander/r4");
        if (xTaskCreate(ArduinoTelnetTransport::taskBody, "telnet", STACK_TELNET, &telnet, 2, &telnetHandle) != pdPASS) {
            Serial.println("[err] telnet task"); return;
        }
    } else {
        Serial.println("[wifi] failed — telnet and OTA unavailable");
    }

    vTaskStartScheduler();
}

void loop() {}
