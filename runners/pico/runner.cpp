#include "commander.h"
#include "BootselModule.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hal/hal.h"
#include "core/WifiHooks.h"
#include "transport/telnet/TelnetTransport.h"
#include <stdio.h>
#include <string.h>
// ota_cmd.h includes lwip/sockets.h which defines poll() as a macro.
// Must come after pico/cyw43_arch.h (which uses poll as an identifier in
// async_context.h) to avoid the macro clobbering the function pointer name.
#ifdef COMMANDER_ENABLE_OTA
#include "ota_cmd.h"
#endif

static CommanderConfig _cfg;
static CommandRegistry _registry;
static UartTransport   _uart;
static TelnetTransport _telnet;
static BootselModule   _bootsel;

// Set by `wifi off` so the link watchdog (wifiMonitor) doesn't fight the user and
// reconnect a deliberately-disabled radio. Cleared by `wifi on`.
static volatile bool _wifi_user_off = false;

// ── FreeRTOS panic hooks ──────────────────────────────────────────────────
// These run from the tick ISR — printf is unsafe (may deadlock on the stdio
// spinlock). Use watchdog scratch[6] to pass the panic type across the reset
// boundary; main() reads and prints it after USB CDC is up.
#define PANIC_MAGIC_MALLOC 0x0BAD0001u
#define PANIC_MAGIC_STACK  0x0BAD0002u
#define PANIC_MAGIC_CMDID  0x0BAD0003u

extern "C" {
    void vApplicationMallocFailedHook(void) {
        watchdog_hw->scratch[6] = PANIC_MAGIC_MALLOC;
        watchdog_reboot(0, 0, 0);
        for (;;) {}
    }
    void vApplicationStackOverflowHook(TaskHandle_t, char *) {
        watchdog_hw->scratch[6] = PANIC_MAGIC_STACK;
        watchdog_reboot(0, 0, 0);
        for (;;) {}
    }
    void vApplicationIdleHook(void) {}
}

void commander_on_panic() {
    watchdog_hw->scratch[6] = PANIC_MAGIC_CMDID;
    watchdog_reboot(0, 0, 0);
    for (;;) {}
}

// ── Weak hook defaults ────────────────────────────────────────────────────
// Apps override any of these by defining the same symbol without __weak__.
extern "C" __attribute__((weak)) void commander_early_init()                   {}
extern "C" __attribute__((weak)) void commander_on_uart_ready(UartTransport &) {}
extern "C" __attribute__((weak)) void commander_on_wifi_connected()            {}

// ── WiFi control hooks (for the `wifi` module) ────────────────────────────────
// cyw43 + lwIP access is serialized by the async-context lock, so these are safe
// to call from the shell task. on() reconnects with the configured credentials.
extern "C" bool commander_wifi_status(WifiInfo *info) {
    cyw43_arch_lwip_begin();
    info->connected = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
    info->ssid[0] = '\0';
    if (_cfg.wifi_ssid) { strncpy(info->ssid, _cfg.wifi_ssid, sizeof(info->ssid) - 1); }
    info->ip[0] = '\0';
    if (info->connected && netif_default) {
        const char *ip = ip4addr_ntoa(netif_ip4_addr(netif_default));
        if (ip) strncpy(info->ip, ip, sizeof(info->ip) - 1);
    }
    info->rssi = 0;
    int32_t rssi = 0;
    if (info->connected && cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) info->rssi = rssi;
    cyw43_arch_lwip_end();
    return info->connected;
}
// ── Scanning (core/WifiHooks.h) ──────────────────────────────────────────────
//
// cyw43_wifi_scan is already asynchronous: it calls back per beacon heard, and
// cyw43_wifi_scan_active() reports completion. What this adds is a stable result
// set — the driver reports the same BSSID repeatedly as it re-hears it, so
// results are deduplicated by BSSID keeping the strongest sighting, and kept
// sorted strongest-first so a caller that only has room for a few gets the ones
// that matter.
#ifndef COMMANDER_WIFI_MAX_APS
#define COMMANDER_WIFI_MAX_APS 32
#endif

static WifiAp   _aps[COMMANDER_WIFI_MAX_APS];
static unsigned _ap_count = 0;

static int _scan_cb(void *, const cyw43_ev_scan_result_t *r) {
    if (!r) return 0;
    // Same radio heard again: keep the strongest reading rather than the latest.
    for (unsigned i = 0; i < _ap_count; ++i) {
        if (memcmp(_aps[i].bssid, r->bssid, 6) == 0) {
            if (r->rssi > _aps[i].rssi) _aps[i].rssi = r->rssi;
            return 0;
        }
    }
    if (_ap_count >= COMMANDER_WIFI_MAX_APS) {
        // Table full: displace the weakest, but only for something stronger.
        unsigned weakest = 0;
        for (unsigned i = 1; i < _ap_count; ++i)
            if (_aps[i].rssi < _aps[weakest].rssi) weakest = i;
        if (r->rssi <= _aps[weakest].rssi) return 0;
        _ap_count = weakest;                 // overwrite that slot below
    }
    WifiAp &a = _aps[_ap_count];
    unsigned n = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(a.ssid, r->ssid, n);
    a.ssid[n] = '\0';
    memcpy(a.bssid, r->bssid, 6);
    a.rssi    = r->rssi;
    a.channel = (uint8_t)r->channel;
    a.auth    = r->auth_mode;
    if (_ap_count < COMMANDER_WIFI_MAX_APS) ++_ap_count;
    return 0;
}

extern "C" bool commander_wifi_scan_start() {
    cyw43_arch_lwip_begin();
    bool busy = cyw43_wifi_scan_active(&cyw43_state);
    bool ok = false;
    if (!busy) {
        _ap_count = 0;                       // results belong to one scan only
        cyw43_wifi_scan_options_t opts;
        memset(&opts, 0, sizeof(opts));
        ok = cyw43_wifi_scan(&cyw43_state, &opts, nullptr, _scan_cb) == 0;
    }
    cyw43_arch_lwip_end();
    return ok;
}

extern "C" bool commander_wifi_scan_busy() {
    cyw43_arch_lwip_begin();
    bool busy = cyw43_wifi_scan_active(&cyw43_state);
    cyw43_arch_lwip_end();
    return busy;
}

extern "C" unsigned commander_wifi_scan_results(WifiAp *out, unsigned max) {
    if (!out || !max) return 0;
    cyw43_arch_lwip_begin();
    unsigned n = _ap_count < max ? _ap_count : max;
    // Selection sort by RSSI, strongest first. n is <= 32 and this runs once per
    // read, not per beacon, so the simple thing is the right thing.
    for (unsigned i = 0; i < n; ++i) {
        unsigned best = i;
        for (unsigned j = i + 1; j < _ap_count; ++j)
            if (_aps[j].rssi > _aps[best].rssi) best = j;
        if (best != i) { WifiAp t = _aps[i]; _aps[i] = _aps[best]; _aps[best] = t; }
        out[i] = _aps[i];
    }
    cyw43_arch_lwip_end();
    return n;
}

extern "C" void commander_wifi_off() {
    _wifi_user_off = true;                 // suppress watchdog auto-reconnect
    cyw43_arch_disable_sta_mode();
}
extern "C" void commander_wifi_on() {
    if (!_cfg.wifi_ssid) return;
    _wifi_user_off = false;
    cyw43_arch_enable_sta_mode();
    cyw43_arch_wifi_connect_async(_cfg.wifi_ssid, _cfg.wifi_password, CYW43_AUTH_WPA2_MIXED_PSK);
}

#ifdef COMMANDER_ENABLE_CONTROLLER
#include "modules/controller/ControllerModule.h"
// Called once after the controller module is registered (the cmdr-generated file
// emits the call). Weak default does nothing; apps override it to wire input —
// e.g. controller.onUpdate(...) to map sticks to drive, or set up bindings.
// C++ linkage (not extern "C") so the ControllerModule& parameter type matches.
__attribute__((weak)) void commander_on_controller_ready(ControllerModule &) {}
#endif

// Bring up the network services after the FIRST successful association — whether
// that's the initial boot connect or a later watchdog reconnect. mDNS + the telnet
// listener are set up once (idempotent); the app hook runs on every (re)connect.
// Without this, a boot where the initial connect failed and the watchdog recovered
// the link would have WiFi up but no mDNS/telnet (they only ran in the initial path).
static bool _wifi_services_up = false;
static void wifiBringUpServices() {
    bool first = !_wifi_services_up;
    cyw43_arch_lwip_begin();
    if (first) {
        mdns_resp_init();
        mdns_resp_add_netif(netif_default, _cfg.hostname);
    }
    // Re-announce on EVERY (re)connect. A link bounce or a fresh DHCP lease leaves the
    // mDNS responder silent — `<host>.local` stops resolving even though WiFi is up —
    // until it re-advertises. (lwIP: call this from the netif status callback; we do it
    // on each reconnect instead.) This is the bug behind "WiFi up but no mDNS" after an
    // OTA reboot or a watchdog recovery.
    mdns_resp_restart(netif_default);
    cyw43_arch_lwip_end();
    if (first) {
        if (_cfg.enable_telnet) {
            const char *tgreeting = _cfg.telnet_greeting ? _cfg.telnet_greeting : _cfg.hostname;
            _telnet.begin(_registry, tgreeting);
            xTaskCreate(TelnetTransport::taskBody, "telnet", 4096, &_telnet, 2, nullptr);
        }
        _wifi_services_up = true;
        if (_cfg.debug) printf("[wifi] services up (mdns+telnet)\n");
    } else if (_cfg.debug) {
        printf("[wifi] mdns re-announced\n");
    }
    commander_on_wifi_connected();      // app hook: every (re)connect
}

// ── WiFi link watchdog ────────────────────────────────────────────────────
// The Pico connects once at boot; if that link later drops (AP reboot, RF glitch,
// roaming) nothing re-establishes it and telnet/mDNS go dark until a reflash.
// Poll the STA link and reconnect when it's down — unless the user ran `wifi off`.
// On the first successful (re)connect this also brings up mDNS + telnet, in case the
// initial boot connect never succeeded (e.g. AP slow after an OTA reboot).
static void wifiMonitor() {
    bool announced = false;   // log the first down + the recovery, not every poll
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (_wifi_user_off) continue;

        cyw43_arch_lwip_begin();
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
        if (link == CYW43_LINK_UP) {
            if (announced) { printf("[wifi] reconnected\n"); announced = false; }
            continue;
        }

        if (!announced) { printf("[wifi] link down (%d) — reconnecting...\n", link); announced = true; }
        cyw43_arch_enable_sta_mode();
        int err = cyw43_arch_wifi_connect_timeout_ms(
            _cfg.wifi_ssid, _cfg.wifi_password, CYW43_AUTH_WPA2_MIXED_PSK, 15000);
        if (err == 0) wifiBringUpServices();   // mDNS/telnet (if not yet) + app hook
        // else: next poll retries (the 5 s delay above paces the attempts)
    }
}

// ── Main FreeRTOS task ────────────────────────────────────────────────────
static void runnerTask(void *) {
    if (_cfg.i2c_sda >= 0)
        hal_i2c_init((uint8_t)_cfg.i2c_sda, (uint8_t)_cfg.i2c_scl, _cfg.i2c_hz);

    // One CYW43 bring-up, shared by WiFi and Bluetooth (the combined firmware
    // does both). It must run before commander_setup() so a controller module's
    // Bluepad32 backend can uni_init() onto the BTstack async-context that
    // cyw43_arch_init() starts. WiFi connect (below) reuses this same init.
#ifdef COMMANDER_ENABLE_CONTROLLER
    const bool need_cyw43 = true;
#else
    const bool need_cyw43 = (_cfg.wifi_ssid != nullptr);
#endif
    bool cyw43_ok = false;
    if (need_cyw43) {
        cyw43_ok = (cyw43_arch_init() == 0);
        if (!cyw43_ok) printf("[cyw43] init failed\n");
    }

    _registry.registerModule(_bootsel);
    commander_setup(_registry);
#ifdef COMMANDER_ENABLE_OTA
    pfb_firmware_commit();
    _registry.registerCommand(CMD("ota", "flash firmware from URL (http)", I2C_NONE, cmdOta, nullptr));
#endif
    _registry.validateIds();

    commander_on_uart_ready(_uart);
    xTaskCreate(UartTransport::taskBody, "uart", 1024, &_uart, 2, nullptr);

    if (_cfg.wifi_ssid && cyw43_ok) {
        printf("[wifi] ssid='%s' connecting...\n", _cfg.wifi_ssid);
        {   // CYW43 already initialized above (shared with Bluetooth)
            cyw43_arch_enable_sta_mode();
            int err = -1;
            for (int i = 0; i < 3 && err != 0; i++) {
                if (i > 0) { printf("[wifi] retry %d...\n", i); vTaskDelay(pdMS_TO_TICKS(5000)); }
                err = cyw43_arch_wifi_connect_timeout_ms(
                    _cfg.wifi_ssid, _cfg.wifi_password,
                    CYW43_AUTH_WPA2_MIXED_PSK, 15000);
                printf("[wifi] connect=%d\n", err);
            }
            if (err == 0) {
                wifiBringUpServices();      // mDNS + telnet + app hook
                if (_cfg.debug) printf("[wifi] ready (mdns+telnet up)\n");
            } else {
                printf("[wifi] connect failed (%d) — watchdog will keep trying\n", err);
            }
        }
        // Keep this task alive as the link watchdog (reuses its proven 8 KB stack,
        // which the blocking connect call needs) instead of deleting it.
        wifiMonitor();   // never returns
    }

    vTaskDelete(nullptr);
}

// ── Entry point ───────────────────────────────────────────────────────────
int main() {
    BootselModule::checkAtBoot();
    commander_early_init();
    _cfg = commander_config();

    stdio_init_all();
    sleep_ms(1000);  // let USB CDC enumerate

    uint32_t panic_code = watchdog_hw->scratch[6];
    watchdog_hw->scratch[6] = 0;
    if (panic_code == PANIC_MAGIC_MALLOC) printf("[PANIC] malloc failed — rebooted\n");
    if (panic_code == PANIC_MAGIC_STACK)  printf("[PANIC] stack overflow — rebooted\n");
    if (panic_code == PANIC_MAGIC_CMDID)  printf("[PANIC] duplicate command ID — rebooted\n");

    _uart.begin(_registry, _cfg.uart_baud, _cfg.uart_greeting);

    xTaskCreate(runnerTask, "main", 8192, nullptr, 1, nullptr);
    vTaskStartScheduler();
    for (;;) {}
}
