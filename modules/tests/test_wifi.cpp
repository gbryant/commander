// WifiModule — the `wifi` command's parsing and formatting, against stub hooks.
//
// The hooks are the platform seam (core/WifiHooks.h), so a host test can supply
// them directly and exercise everything above: subcommand dispatch, the RSSI
// quality bands, and the scan/aps split.
//
// The band boundaries are worth pinning down. -67 dBm is the one that matters —
// below it real-time traffic starts to suffer — and a survey tool that draws
// that line in the wrong place tells you to move a router that was fine.
#include "core/CommandRegistry.h"
#include "core/WifiHooks.h"
#include "modules/WifiModule.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_fails = 0;
static void check(bool cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); ++g_fails; }
}

// ── stub platform ────────────────────────────────────────────────────────────
static bool     g_connected = true;
static int32_t  g_rssi      = -55;
static bool     g_busy      = false;
static bool     g_supported = true;
static bool     g_started   = false;
static WifiAp   g_aps[8];
static unsigned g_ap_count  = 0;

extern "C" bool commander_wifi_status(WifiInfo *info) {
    memset(info, 0, sizeof(*info));
    info->connected = g_connected;
    strcpy(info->ssid, "testnet");
    strcpy(info->ip, "10.0.0.5");
    info->rssi = g_rssi;
    return g_connected;
}
extern "C" void commander_wifi_off() { g_connected = false; }
extern "C" void commander_wifi_on()  { g_connected = true; }
extern "C" bool commander_wifi_scan_start() {
    if (!g_supported || g_busy) return false;
    g_started = true; return true;
}
extern "C" bool commander_wifi_scan_busy() { return g_busy; }
extern "C" unsigned commander_wifi_scan_results(WifiAp *out, unsigned max) {
    unsigned n = g_ap_count < max ? g_ap_count : max;
    for (unsigned i = 0; i < n; ++i) out[i] = g_aps[i];
    return n;
}

// ── a Writer that collects into a string ─────────────────────────────────────
struct CaptureWriter : Writer {
    std::string text;
    void write(const char *s) override { text += s; }
    void writeln(const char *s = "") override { text += s; text += "\n"; }
};

static std::string run(CommandRegistry &reg, const char *line) {
    CaptureWriter w;
    reg.dispatch(line, w);
    return w.text;
}

static bool has(const std::string &h, const char *n) {
    return h.find(n) != std::string::npos;
}

static void addAp(const char *ssid, int16_t rssi, uint8_t ch) {
    WifiAp &a = g_aps[g_ap_count];
    memset(&a, 0, sizeof(a));
    strncpy(a.ssid, ssid, 32);
    a.rssi = rssi; a.channel = ch;
    for (int i = 0; i < 6; ++i) a.bssid[i] = (uint8_t)(0x10 * (g_ap_count + 1) + i);
    ++g_ap_count;
}

int main() {
    printf("WifiModule\n");
    CommandRegistry reg;
    WifiModule wifi;
    reg.registerModule(wifi);

    // ── status ───────────────────────────────────────────────────────────────
    std::string s = run(reg, "wifi status");
    check(has(s, "connected"), "status reports connected");
    check(has(s, "testnet") && has(s, "10.0.0.5"), "status reports ssid and ip");
    check(has(s, "-55 dBm"), "status reports rssi");
    check(has(s, "very good"), "-55 dBm reads as 'very good'");
    check(run(reg, "wifi") == s, "bare 'wifi' equals 'wifi status'");

    // ── the quality bands, at their boundaries ───────────────────────────────
    struct { int32_t rssi; const char *word; } bands[] = {
        { -30, "excellent" }, { -50, "excellent" },
        { -51, "very good" }, { -60, "very good" },
        { -61, "good"      }, { -67, "good"      },   // -67: the threshold that matters
        { -68, "fair"      }, { -70, "fair"      },
        { -71, "weak"      }, { -80, "weak"      },
        { -81, "unusable"  }, { -95, "unusable"  },
    };
    for (auto &b : bands) {
        g_rssi = b.rssi;
        char why[64];
        snprintf(why, sizeof(why), "%d dBm reads as '%s'", (int)b.rssi, b.word);
        check(has(run(reg, "wifi status"), b.word), why);
    }
    g_rssi = -55;

    // ── disconnected ─────────────────────────────────────────────────────────
    g_connected = false;
    check(has(run(reg, "wifi status"), "disconnected"), "reports disconnected");
    g_connected = true;

    // ── scan ─────────────────────────────────────────────────────────────────
    check(has(run(reg, "wifi scan"), "scanning"), "scan starts");
    check(g_started, "scan reached the platform hook");

    g_busy = true;
    check(has(run(reg, "wifi scan"), "already running"), "a second scan is refused, not stacked");
    check(has(run(reg, "wifi aps"), "in progress"), "aps says so while a scan runs");
    g_busy = false;

    check(has(run(reg, "wifi aps"), "no scan results"), "aps is honest about an empty table");

    g_supported = false;
    check(has(run(reg, "wifi scan"), "not supported"),
          "a platform that cannot scan says so, rather than reporting no networks");
    g_supported = true;

    // ── aps listing ──────────────────────────────────────────────────────────
    addAp("strong-net", -42, 6);
    addAp("far-net",    -78, 11);
    addAp("",           -60, 1);          // hidden SSID
    s = run(reg, "wifi aps");
    check(has(s, "strong-net") && has(s, "far-net"), "aps lists networks");
    check(has(s, "-42") && has(s, "-78"), "aps lists rssi");
    check(has(s, "excellent") && has(s, "weak"), "aps labels quality per network");
    check(has(s, "(hidden)"), "a blank ssid renders as (hidden), not as nothing");
    check(has(s, "10:11:12:13:14:15"), "aps prints the bssid, which tells mesh nodes apart");
    check(s.find("strong-net") < s.find("far-net"), "results keep the order given (strongest first)");

    // ── unknown subcommand ───────────────────────────────────────────────────
    check(has(run(reg, "wifi wibble"), "status | off | on | scan | aps"),
          "an unknown subcommand prints usage");

    printf(g_fails ? "  %d FAILED\n" : "  all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
