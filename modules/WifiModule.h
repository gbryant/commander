#pragma once
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "core/WifiHooks.h"
#include <string.h>
#include <stdint.h>

// `wifi status | off | on` — inspect and control WiFi from the shell. Portable;
// the platform specifics live behind the hooks above.
class WifiModule : public IModule {
public:
    const char *name() const override { return "wifi"; }
    void        init() override {}
    void        registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD("wifi", "WiFi status/control - 'wifi status|off|on'",
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
            if (w.rssi) { out.write("  rssi="); putInt(out, w.rssi); out.write(" dBm"); }
            out.writeln();
            return;
        }
        if (isTok(args, "off")) { commander_wifi_off(); out.writeln("ok: wifi off"); return; }
        if (isTok(args, "on"))  { commander_wifi_on();  out.writeln("ok: wifi on (reconnecting)"); return; }
        out.writeln("wifi: status | off | on");
    }
};
