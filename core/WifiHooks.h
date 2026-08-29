#pragma once
#include <stdint.h>

// WiFi state snapshot + control hooks. WiFi lives in the per-platform runner
// (it owns the credentials and, on the R4, the single modem-owning task), so the
// runner implements these; the portable `wifi` command (modules/WifiModule.h)
// calls them. Only runners on WiFi platforms (pico/pico2/r4/esp32) provide them,
// which is also where the `wifi` module is enable-able — so no weak defaults.
struct WifiInfo {
    bool    connected;
    char    ssid[33];
    char    ip[16];
    int32_t rssi;       // dBm; 0 if unknown
};

extern "C" bool commander_wifi_status(WifiInfo *info);
extern "C" void commander_wifi_off();
extern "C" void commander_wifi_on();

// ── Scanning ─────────────────────────────────────────────────────────────────
//
// `commander_wifi_status()` reports the link to the AP this station is
// *associated* with, which is the right number for "how good is my connection"
// and the wrong one for "where should the router go". A station roams lazily:
// walk from one mesh node towards another and the RSSI keeps falling long after
// a phone would have handed over, so a survey built on it alone reads healthy
// coverage as a dead spot. Scanning sees every AP in earshot, each with its own
// BSSID — which is what tells two nodes of one mesh apart.
//
// **Asynchronous on purpose.** A scan takes seconds; nothing in commander may
// block the shell for that long. Start one, poll `busy()`, then read the
// results — the same shape as every other long operation here.
//
//     if (commander_wifi_scan_start()) { ... }
//     if (!commander_wifi_scan_busy()) {
//         WifiAp aps[16];
//         size_t n = commander_wifi_scan_results(aps, 16);
//     }
//
// Results persist until the next scan starts, so they can be read repeatedly.
// Weak defaults report "unsupported" so runners without an implementation still
// link and say so honestly, rather than failing to build.
struct WifiAp {
    char    ssid[33];   // NUL-terminated; empty for a hidden network
    uint8_t bssid[6];   // the radio's MAC — what distinguishes two mesh nodes
    int16_t rssi;       // dBm
    uint8_t channel;
    uint8_t auth;       // 0 = open; otherwise platform's auth enum
};

// Begin a scan. False if one is already running or the platform can't scan.
extern "C" bool commander_wifi_scan_start();
// True while a scan is in flight. Results are stable once this goes false.
extern "C" bool commander_wifi_scan_busy();
// Copy up to `max` results out, strongest first. Returns how many were written.
extern "C" unsigned commander_wifi_scan_results(WifiAp *out, unsigned max);
