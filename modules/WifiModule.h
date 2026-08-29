#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/WifiHooks.h"
#include <string.h>
#include <stdint.h>

// `wifi status | off | on | scan | aps` — inspect and control WiFi from the
// shell. Portable; the platform specifics live behind the hooks above.
//
// `scan` and `aps` are two commands rather than one because a scan takes
// seconds and nothing here may block: `scan` starts one and returns, `aps`
// prints whatever the last completed scan found. Both are subcommands of the
// single `wifi` command, so adding them costs no registry slot.
class WifiModule : public IModule {
public:
    const char *name() const override { return "wifi"; }
    void        init() override {}
    void        registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD("wifi", "WiFi status/scan - 'wifi status|off|on|scan|aps'",
                                I2C_NONE, wifiCmd, nullptr));
    }

private:
    static void putInt(Writer &out, int32_t v) {
        char tmp[12]; int t = 0;
        bool neg = v < 0;
        uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
        if (u == 0) tmp[t++] = '0';
        while (u) { tmp[t++] = (char)('0' + u % 10); u /= 10; }
        char s[13]; int i = 0;
        if (neg) s[i++] = '-';
        while (t) s[i++] = tmp[--t];
        s[i] = '\0';
        out.write(s);
    }

    static void putPad(Writer &out, const char *s, int width) {
        int n = 0;
        if (s) { out.write(s); while (s[n]) ++n; }
        while (n++ < width) out.write(" ");
    }

    static void putHex8(Writer &out, uint8_t v) {
        static const char *H = "0123456789abcdef";
        char b[3] = { H[v >> 4], H[v & 0xF], '\0' };
        out.write(b);
    }

    // -30 excellent ... -90 unusable. The -67 boundary is the one that matters:
    // below it, real-time traffic (voice, video) starts to suffer.
    static const char *quality(int32_t rssi) {
        if (rssi >= -50) return "excellent";
        if (rssi >= -60) return "very good";
        if (rssi >= -67) return "good";
        if (rssi >= -70) return "fair";
        if (rssi >= -80) return "weak";
        return "unusable";
    }

    static void apsCmd(Writer &out) {
        if (commander_wifi_scan_busy()) { out.writeln("wifi: scan in progress"); return; }
        WifiAp aps[16];
        unsigned n = commander_wifi_scan_results(aps, 16);
        if (n == 0) {
            out.writeln("wifi: no scan results - run 'wifi scan' first");
            return;
        }
        out.writeln("  ssid                              ch   dBm  quality    bssid");
        out.writeln("  --------------------------------  ---  ----  ---------  -----------------");
        for (unsigned i = 0; i < n; ++i) {
            out.write("  ");
            putPad(out, aps[i].ssid[0] ? aps[i].ssid : "(hidden)", 34);
            char ch[5]; int c = 0;
            uint8_t v = aps[i].channel;
            if (v >= 100) ch[c++] = (char)('0' + v / 100);
            if (v >= 10)  ch[c++] = (char)('0' + (v / 10) % 10);
            ch[c++] = (char)('0' + v % 10);
            ch[c] = '\0';
            putPad(out, ch, 5);
            putInt(out, aps[i].rssi); out.write("  ");
            putPad(out, quality(aps[i].rssi), 11);
            for (int b = 0; b < 6; ++b) {
                if (b) out.write(":");
                putHex8(out, aps[i].bssid[b]);
            }
            out.writeln();
        }
    }

    static bool isTok(const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    }

    static void wifiCmd(const char *args, Writer &out, void *) {
        while (*args == ' ') ++args;
        if (*args == '\0' || isTok(args, "status")) {
            WifiInfo w; memset(&w, 0, sizeof(w));
            if (!commander_wifi_status(&w)) { out.writeln("wifi: disconnected"); return; }
            out.write("wifi: connected  ssid="); out.write(w.ssid[0] ? w.ssid : "?");
            out.write("  ip="); out.write(w.ip[0] ? w.ip : "?");
            if (w.rssi) {
                out.write("  rssi="); putInt(out, w.rssi); out.write(" dBm (");
                out.write(quality(w.rssi)); out.write(")");
            }
            out.writeln();
            return;
        }
        if (isTok(args, "off")) { commander_wifi_off(); out.writeln("ok: wifi off"); return; }
        if (isTok(args, "on"))  { commander_wifi_on();  out.writeln("ok: wifi on (reconnecting)"); return; }
        if (isTok(args, "scan")) {
            if (commander_wifi_scan_busy()) { out.writeln("wifi: scan already running"); return; }
            if (!commander_wifi_scan_start()) {
                out.writeln("wifi: scan not supported on this platform");
                return;
            }
            out.writeln("ok: scanning - 'wifi aps' for results (a few seconds)");
            return;
        }
        if (isTok(args, "aps")) { apsCmd(out); return; }
        out.writeln("wifi: status | off | on | scan | aps");
    }
};
