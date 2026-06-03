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
