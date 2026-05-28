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
static WiFiUDP                _mdns_udp;

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

// ── Minimal mDNS A-record responder ──────────────────────────────────────────
// Joins 224.0.0.251:5353 and responds to A/ANY queries for <hostname>.local.
// No external library needed — uses WiFiUDP multicast (AT+UDPBEGINMULTI).

static const IPAddress kMdnsGroup(224, 0, 0, 251);
static const uint16_t  kMdnsPort = 5353;

// Skip one DNS name (length-prefixed labels or pointer); returns position after.
static int mdns_skip_name(const uint8_t* buf, int len, int pos) {
    while (pos < len) {
        if (buf[pos] == 0) return pos + 1;
        if ((buf[pos] & 0xC0) == 0xC0) return pos + 2;
        pos += 1 + buf[pos];
    }
    return len;
}

// Returns true if the uncompressed DNS name at buf[pos] is "<hostname>.local."
static bool mdns_name_match(const uint8_t* buf, int len, int pos) {
    if (!_cfg.hostname) return false;
    int hl = (int)strlen(_cfg.hostname);
    if (pos + 1 + hl + 1 + 5 + 1 > len) return false;
    if (buf[pos] != (uint8_t)hl) return false;
    if (memcmp(buf + pos + 1, _cfg.hostname, hl) != 0) return false;
    pos += 1 + hl;
    if (buf[pos] != 5 || memcmp(buf + pos + 1, "local", 5) != 0) return false;
    return buf[pos + 6] == 0;
}

// Send an unsolicited (or query-triggered) A record for <hostname>.local.
static void mdns_announce() {
    if (!_cfg.hostname) return;
    IPAddress ip = WiFi.localIP();
    int hl = (int)strlen(_cfg.hostname);
    uint8_t resp[128]; int r = 0;
    resp[r++]=0x00; resp[r++]=0x00;            // ID = 0
    resp[r++]=0x84; resp[r++]=0x00;            // QR=1, AA=1
    resp[r++]=0x00; resp[r++]=0x00;            // QDCOUNT = 0
    resp[r++]=0x00; resp[r++]=0x01;            // ANCOUNT = 1
    resp[r++]=0x00; resp[r++]=0x00;            // NSCOUNT = 0
    resp[r++]=0x00; resp[r++]=0x00;            // ARCOUNT = 0
    resp[r++]=(uint8_t)hl;
    memcpy(resp+r, _cfg.hostname, hl); r += hl;
    resp[r++]=5; memcpy(resp+r, "local", 5); r += 5;
    resp[r++]=0;                               // root label
    resp[r++]=0x00; resp[r++]=0x01;            // TYPE A
    resp[r++]=0x80; resp[r++]=0x01;            // CLASS IN + cache-flush
    resp[r++]=0x00; resp[r++]=0x00; resp[r++]=0x00; resp[r++]=120; // TTL 120 s
    resp[r++]=0x00; resp[r++]=0x04;            // RDLENGTH = 4
    resp[r++]=ip[0]; resp[r++]=ip[1]; resp[r++]=ip[2]; resp[r++]=ip[3];
    _mdns_udp.beginMulticastPacket();
    _mdns_udp.write(resp, r);
    _mdns_udp.endPacket();
}

// Check for incoming mDNS queries and reply to A/ANY for <hostname>.local.
static void mdns_run() {
    int len = _mdns_udp.parsePacket();
    if (len < 12 || len > 255) return;
    uint8_t buf[256];
    len = _mdns_udp.read(buf, sizeof(buf));
    if ((buf[2] & 0xF8) != 0) return;      // not a standard query
    int qdcount = (buf[4] << 8) | buf[5];
    int pos = 12;
    for (int q = 0; q < qdcount && pos < len; q++) {
        int name_pos = pos;
        pos = mdns_skip_name(buf, len, pos);
        if (pos + 4 > len) break;
        uint16_t qtype  = ((uint16_t)buf[pos] << 8) | buf[pos+1];
        uint16_t qclass = ((uint16_t)buf[pos+2] << 8) | buf[pos+3];
        pos += 4;
        if ((qtype == 1 || qtype == 255) && (qclass & 0x7FFFu) == 1
                && mdns_name_match(buf, len, name_pos)) {
            mdns_announce();
            break;
        }
    }
}

static void mdnsTask(void*) {
    for (;;) {
        mdns_run();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
// ─────────────────────────────────────────────────────────────────────────────

static void wifiTask(void *) {
    // Match the UART task's 1500ms settle — Serial must be ready before first print.
    // WiFi SPI is slow on R4 (~5x slower than R3); running in a task keeps the
    // UART task and USB interrupt handler live during the connection loop.
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

        // Telnet first — must not be blocked by mDNS setup.
        if (_cfg.enable_telnet) {
            const char *tg = _cfg.telnet_greeting ? _cfg.telnet_greeting : _cfg.hostname;
            _server.begin();
            _telnet.begin(_registry, _server, tg);
            xTaskCreate(ArduinoTelnetTransport::taskBody, "telnet", 256, &_telnet, 2, nullptr);
        }

        // setHostname after connect: the ESP32-S3 runs ESP-IDF which has native
        // mDNS; calling AT+HOSTNAME once the modem is fully up may enable it.
        // Also start the inline responder to handle A-record queries directly.
        if (_cfg.hostname) {
            WiFi.setHostname(_cfg.hostname);
            _mdns_udp.beginMulticast(kMdnsGroup, kMdnsPort);
            mdns_announce();
            xTaskCreate(mdnsTask, "mdns", 512, nullptr, 1, nullptr);
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
