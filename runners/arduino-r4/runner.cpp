#ifdef COMMANDER_R4_RUNNER

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <WiFiS3.h>
#ifdef COMMANDER_R4_OTA
#include <ArduinoOTA.h>     // enabled via `cmdr enable ota`
#endif
#include "commander.h"
#include "hal/hal.h"
#include "core/WifiHooks.h"
// Unity-build: only compiled for R4, keeps it out of library srcFilter
#include "transport/telnet/arduino/ArduinoTelnetTransport.cpp"

static CommanderConfig        _cfg;
static CommandRegistry        _registry;
static UartTransport          _uart;
static ArduinoTelnetTransport _telnet;
static WiFiServer             _server(23);
static WiFiUDP                _mdns_udp;

// WiFi control for the `wifi` module. WiFiS3's modem has no locking, so the
// command (UART/telnet task) only sets flags / reads a cache — net_poll (the one
// task that owns the modem) does the actual work and refreshes _wifi_cache.
static volatile bool _wifi_off_req  = false;
static volatile bool _wifi_on_req   = false;
static volatile bool _wifi_user_off = false;   // suppresses auto-reconnect after `wifi off`
static WifiInfo      _wifi_cache    = {};

extern "C" bool commander_wifi_status(WifiInfo *info) { *info = _wifi_cache; return info->connected; }
extern "C" void commander_wifi_off() { _wifi_off_req = true; }
extern "C" void commander_wifi_on()  { _wifi_on_req  = true; }

extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

// FreeRTOS fault hooks. Active when the project's platformio.ini sets
// configCHECK_FOR_STACK_OVERFLOW=2 and configUSE_MALLOC_FAILED_HOOK=1. Without
// them, a stack overflow or heap exhaustion corrupts memory silently and can
// wedge the ESP32-S3 bridge (no serial, slow-pulse LED, needs a power cycle).
// With them, the offending task is named on Serial before the board halts.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *pcTaskName) {
    Serial.println();
    Serial.print("!! FreeRTOS STACK OVERFLOW in task: ");
    Serial.println(pcTaskName);
    Serial.flush();
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

extern "C" void vApplicationMallocFailedHook(void) {
    Serial.println();
    Serial.println("!! FreeRTOS heap exhausted (pvPortMalloc failed)");
    Serial.flush();
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

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
static void mdns_run(int len) {
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

#ifdef COMMANDER_R4_OTA
// On-demand OTA. The `ota start` command only sets this flag; the actual
// handoff runs from net_poll — i.e. in the networking task — so it never races
// the modem (the single-task rule). WiFiS3 has a tiny socket pool, so OTA and
// telnet cannot listen at once: we free the telnet + mDNS sockets first, then
// listen for an ArduinoOTA push on :65280. ArduinoOTA reboots on success; on
// timeout we reset to restore the normal telnet/mDNS state cleanly.
static volatile bool _ota_arm = false;

static void run_ota() {
    Serial.println("[ota] closing telnet + mDNS, listening 60s on :65280...");
    _server.end();
    _mdns_udp.stop();
    const char *host = _cfg.hostname ? _cfg.hostname : "commander";
    ArduinoOTA.begin(WiFi.localIP(), host, "", InternalStorage);
    uint32_t t0 = millis();
    while (millis() - t0 < 60000) {
        ArduinoOTA.poll();   // reboots the board on a successful upload
        delay(20);
    }
    Serial.println("[ota] timeout — rebooting to restore telnet");
    NVIC_SystemReset();
}

static void otaCmd(const char *args, Writer &out, void *) {
    while (*args == ' ') ++args;
    if (strncmp(args, "start", 5) == 0) {
        out.writeln("ok: arming OTA — telnet will drop; push within 60s");
        _ota_arm = true;     // handed off to net_poll (networking task)
    } else {
        out.writeln("ota: 'ota start' arms a firmware update");
        out.writeln("     (closes telnet, listens 60s on :65280, reboots on success)");
    }
}
#endif // COMMANDER_R4_OTA

static bool wifi_attempt();  // defined below; called by net_poll for self-heal

// One mDNS tick, rate-limited to ~100ms. Installed as the telnet task's poll
// hook so a single task owns all modem access — WiFiS3's modem singleton has
// no locking, so polling it from two tasks corrupts AT framing on Serial2.
static void net_poll() {
#ifdef COMMANDER_R4_OTA
    if (_ota_arm) { _ota_arm = false; run_ota(); }   // runs in this (net) task
#endif
    // `wifi off`/`on` requests from the shell (this task owns the modem).
    if (_wifi_off_req) {
        _wifi_off_req = false; _wifi_user_off = true;
        Serial.println("[wifi] off (user)");
        if (_cfg.enable_telnet) _server.end();
        _mdns_udp.stop();
        WiFi.disconnect();
    }
    if (_wifi_on_req) {                              // re-enable; reconnect happens below
        _wifi_on_req = false; _wifi_user_off = false;
    }
    uint32_t now = millis();

    // WiFi self-heal. WiFiS3 does not reconnect on its own, and wifiTask exits
    // after the first association — so a dropped link leaves the hostname
    // unreachable until a physical reset. Re-associate here (this task owns the
    // modem) and rebuild mDNS + the telnet listener, with the same primitives OTA
    // uses to tear them down/back up. The UART shell (its own task) stays usable
    // throughout, and wifi_attempt() blocks only this networking task.
    static uint32_t last_link_check = 0;
    if (now - last_link_check >= 3000) {
        last_link_check = now;
        if (!_wifi_user_off && WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] link lost — reconnecting...");
            if (_cfg.enable_telnet) _server.end();
            _mdns_udp.stop();
            WiFi.disconnect();
            for (int attempt = 1; !wifi_attempt(); attempt++) {
                Serial.print("[wifi] reconnect attempt "); Serial.print(attempt);
                Serial.println(" failed — retrying in 5s");
                WiFi.disconnect();
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            Serial.print("[wifi] reconnected "); Serial.println(WiFi.localIP());
            if (_cfg.hostname && _mdns_udp.beginMulticast(kMdnsGroup, kMdnsPort))
                mdns_announce();
            if (_cfg.enable_telnet) _server.begin();
            now = millis();
        }
        // Refresh the cache the `wifi` command reads (modem reads on this task).
        _wifi_cache.connected = (WiFi.status() == WL_CONNECTED);
        if (_wifi_cache.connected) {
            const char *ss = WiFi.SSID();             // WiFiS3 returns const char*
            strncpy(_wifi_cache.ssid, ss ? ss : "", sizeof(_wifi_cache.ssid) - 1);
            _wifi_cache.ssid[sizeof(_wifi_cache.ssid) - 1] = '\0';
            IPAddress ip = WiFi.localIP();
            snprintf(_wifi_cache.ip, sizeof(_wifi_cache.ip), "%d.%d.%d.%d",
                     ip[0], ip[1], ip[2], ip[3]);
            _wifi_cache.rssi = WiFi.RSSI();
        } else {
            _wifi_cache.ssid[0] = '\0'; _wifi_cache.ip[0] = '\0'; _wifi_cache.rssi = 0;
        }
    }

    static uint32_t last = 0;
    if (now - last < 100) return;
    last = now;
    mdns_run(_mdns_udp.parsePacket());
}

// mDNS-only networking task — used when telnet is disabled but a hostname is set.
static void mdnsTask(void*) {
    for (;;) {
        net_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
// One connection attempt: WiFi.begin then poll up to ~20s for association +
// a valid DHCP lease. Returns true on success.
static bool wifi_attempt() {
    Serial.print("[wifi] connecting");
    WiFi.begin(_cfg.wifi_ssid, _cfg.wifi_password);
    for (int i = 0; i < 40 && (WiFi.status() != WL_CONNECTED ||
                                WiFi.localIP() == IPAddress(0, 0, 0, 0)); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}
// ─────────────────────────────────────────────────────────────────────────────

static void wifiTask(void *) {
    // Wait for the serial link (cap 3s), then settle. On USB-CDC boards `Serial`
    // stays false until the terminal opens (DTR); on the R4's ESP32-S3 UART
    // bridge `operator bool` is always true, so this returns at once. The 1500ms
    // settle that follows is essential: the board just reset when tio attached,
    // and anything printed before the bridge re-establishes UART forwarding is
    // lost — which is why the boot banner is announced here, not in setup().
    // WiFi SPI is also slow on R4 (~5x R3); running in a task keeps the UART
    // task and USB interrupt handler live during the connection loop.
    uint32_t boot_start = millis();
    while (!Serial && (millis() - boot_start) < 3000) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(1500));

    Serial.println();
    Serial.print("=== commander: ");
    Serial.print(_cfg.hostname ? _cfg.hostname : "boot");
    Serial.println(" ===");

    // Retry until associated. A single WiFi.begin often fails transiently on the
    // R4 (modem warm-up, AP timing) — one run reports "failed", the next works.
    // The UART shell runs in its own task, so it stays usable while we retry.
    for (int attempt = 1; !wifi_attempt(); attempt++) {
        Serial.print("[wifi] attempt "); Serial.print(attempt);
        Serial.println(" failed — retrying in 5s");
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    Serial.print("[wifi] ");
    Serial.println(WiFi.localIP());

    // One networking task owns ALL modem access (telnet + mDNS). WiFiS3's
    // modem is a single global with no locking, so two tasks polling it
    // concurrently interleave AT command/response framing on Serial2 and
    // can wedge the ESP32-S3 — which also kills Serial (the UART bridge).
    bool mdns_ok = false;
    if (_cfg.hostname) {
        mdns_ok = _mdns_udp.beginMulticast(kMdnsGroup, kMdnsPort);
        Serial.print("[mdns] "); Serial.println(mdns_ok ? "ok" : "socket failed");
        if (mdns_ok) mdns_announce();
    }

    if (_cfg.enable_telnet) {
        const char *tg = _cfg.telnet_greeting ? _cfg.telnet_greeting : _cfg.hostname;
        _server.begin();
        _telnet.begin(_registry, _server, tg);
        if (mdns_ok) _telnet.setPollFn(net_poll);   // mDNS runs in the telnet task
        xTaskCreate(ArduinoTelnetTransport::taskBody, "net", 512, &_telnet, 2, nullptr);
    } else if (mdns_ok) {
        xTaskCreate(mdnsTask, "mdns", 512, nullptr, 2, nullptr);
    }

    commander_on_wifi_connected();
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
    _registry.registerCommand(CMD("reset", "reboot the firmware", CMD_RESET,
        [](const char *, Writer &out, void *) {
            out.writeln("Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(50));
            NVIC_SystemReset();
        }, nullptr));
#ifdef COMMANDER_R4_OTA
    _registry.registerCommand(CMD("ota", "OTA firmware update ('ota start')", I2C_NONE, otaCmd, nullptr));
#endif
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    xTaskCreate(UartTransport::taskBody, "uart", 256, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid)
        xTaskCreate(wifiTask, "wifi", 512, nullptr, 1, nullptr);

    vTaskStartScheduler();
}

void loop() {}

#endif // COMMANDER_R4_RUNNER
