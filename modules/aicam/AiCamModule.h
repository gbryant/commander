#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // atoi
#include "hal/hal.h"
#include "core/IModule.h"
#include "core/CommandRegistry.h"
#include "modules/aicam/Sscma.h"
#include "i2c_ids.h"

// Grove Vision AI Module V2 (WiseEye2 + camera) host-side module. Talks the SSCMA
// AT protocol to the co-processor over a pluggable transport (UART or I2C — see
// Sscma.h / I2cTransport.h / AiCamUartTransport.h). One namespaced `aicam` command;
// apps get a reference via the weak commander_on_aicam_ready hook to wire results
// to anything (the cmdr-ai-cam demo streams detections + captures images).
//
//   aicam                 status (firmware, current model)
//   aicam info            device ID / name / firmware version
//   aicam model [<id>]    show current model / load model <id>
//   aicam models          list available (flashed) models
//   aicam sensor[s]       current / available sensors (confirms the OV5647)
//   aicam score <0-100>   detection confidence threshold (AT+TSCORE)
//   aicam iou <0-100>     NMS IoU threshold (AT+TIOU)
//   aicam invoke [n]      run inference n times (default 1), print results
//   aicam stream on|off   continuous inference; results print to the console
//   aicam snap [dump]     capture a JPEG (base64); report length / dump it
//   aicam at <command>    raw AT passthrough (probe anything, incl. the SD card)
//   aicam reset           reboot the Vision AI (AT+RST)
class AiCamModule : public IModule {
public:
    explicit AiCamModule(ISscmaTransport &transport)
        : _transport(transport), _client(transport) {}

    const char *name() const override { return "aicam"; }
    void        init()        override { _transport.begin(); }
    void        registerCommands(CommandRegistry &reg) override;

    // Pumped by the UART task (added as a ticker) — drains streaming results.
    void tick() override {
        if (!_streaming) return;
        AiResult res;
        if (_client.poll(res)) {
            if (_cb) _cb(res, _cbCtx);
            else     emitConsole(res);
        }
    }

    // ── C++ API for apps (via commander_on_aicam_ready) ──────────────────────
    SscmaClient &client() { return _client; }
    bool invoke(AiResult &res, int times = 1) {
        char body[24];
        snprintf(body, sizeof(body), "INVOKE=%d,0,1", times);
        return _client.invokeEvent(body, res, 3000);
    }
    void startStream() { _client.writeAt("INVOKE=-1,0,1"); _streaming = true; }
    void stopStream()  { _streaming = false; _client.writeAt("BREAK"); }
    bool streaming() const { return _streaming; }
    // Called for each streamed result (instead of the default console print).
    void onResult(void (*cb)(const AiResult &, void *), void *ctx) { _cb = cb; _cbCtx = ctx; }

    // Format a one-line result summary into buf. Shared by the command + ticker.
    // `add` clamps the running offset so cap - o never goes negative.
    static void format(const AiResult &r, char *buf, int cap) {
        int o = 0;
        auto add = [&](int n) { o += n; if (o > cap - 1) o = cap - 1; };
        add(snprintf(buf + o, cap - o, "det:"));
        for (int i = 0; i < r.nClasses && o < cap - 1; i++)
            add(snprintf(buf + o, cap - o, " cls t%u:%u", r.classes[i].target, r.classes[i].score));
        for (int i = 0; i < r.nBoxes && o < cap - 1; i++)
            add(snprintf(buf + o, cap - o, " box t%u:%u %u,%u %ux%u",
                         r.boxes[i].target, r.boxes[i].score,
                         r.boxes[i].x, r.boxes[i].y, r.boxes[i].w, r.boxes[i].h));
        for (int i = 0; i < r.nPoints && o < cap - 1; i++)
            add(snprintf(buf + o, cap - o, " pt t%u:%u %u,%u",
                         r.points[i].target, r.points[i].score, r.points[i].x, r.points[i].y));
        if (r.nBoxes == 0 && r.nClasses == 0 && r.nPoints == 0)
            add(snprintf(buf + o, cap - o, " (none)"));
        snprintf(buf + o, cap - o, "  perf %u/%u/%ums",
                 r.perf.preprocess, r.perf.inference, r.perf.postprocess);
    }

private:
    ISscmaTransport &_transport;
    SscmaClient _client;
    bool        _streaming = false;
    void      (*_cb)(const AiResult &, void *) = nullptr;
    void       *_cbCtx = nullptr;

    static void emitConsole(const AiResult &res) {
        char line[256];
        format(res, line, sizeof(line));
        hal_uart_puts("\r\n");
        hal_uart_puts(line);
        hal_uart_puts("\r\n");
    }

    static void aicamCmd(const char *args, Writer &out, void *ctx) {
        static_cast<AiCamModule *>(ctx)->dispatch(args, out);
    }
    void dispatch(const char *args, Writer &out);
    void usage(Writer &out);

    static const char *skipSpaces(const char *p) { while (*p == ' ') ++p; return p; }
    static const char *nextTok(const char *p) { while (*p && *p != ' ') ++p; return skipSpaces(p); }
    static bool tokIs(const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    }
    // Run a query and print its response frame. Skips asynchronous log frames
    // (type 2) — the device emits those unprompted (e.g. a stale "Unknown command"
    // from boot), so we wait for the actual command response and print that.
    void query(const char *body, Writer &out, uint32_t timeout = 1500) {
        char frame[AICAM_FRAME_MAX];
        _client.flush();
        _client.writeAt(body);
        uint64_t t0 = hal_time_us();
        while ((uint32_t)((hal_time_us() - t0) / 1000) < timeout) {
            if (_client.readFrame(frame, sizeof(frame), 200) &&
                SscmaClient::frameInt(frame, "type") != 2) {
                out.writeln(frame);
                return;
            }
        }
        out.writeln("timeout");
    }
};

// Weak app hook — the generated commander_modules.h null-checks and calls this
// after registering (header-only module, so a weak *declaration*: unset resolves
// to null and the call is skipped; an app-provided strong definition wins).
extern "C" __attribute__((weak)) void commander_on_aicam_ready(AiCamModule &);

inline void AiCamModule::registerCommands(CommandRegistry &reg) {
    // I2C_NONE: this drives the Vision AI over its own transport, not the shell's
    // I2C relay. One slot, sub-dispatched (info/invoke/stream/snap/at/...).
    reg.registerCommand(CMD("aicam", "Grove Vision AI V2 - 'aicam' status, 'aicam help'",
                            I2C_NONE, aicamCmd, this));
}

inline void AiCamModule::usage(Writer &out) {
    out.writeln("aicam info            device ID / name / firmware version");
    out.writeln("aicam model [<id>]    show / load model");
    out.writeln("aicam models          list flashed models");
    out.writeln("aicam sensor[s]       current / available sensors");
    out.writeln("aicam score <0-100>   confidence threshold");
    out.writeln("aicam iou <0-100>     IoU threshold");
    out.writeln("aicam invoke [n]      run inference, print results");
    out.writeln("aicam stream on|off   continuous inference to console");
    out.writeln("aicam snap [dump]     capture a JPEG (base64)");
    out.writeln("aicam at <command>    raw AT passthrough");
    out.writeln("aicam reset           reboot the Vision AI");
}

inline void AiCamModule::dispatch(const char *args, Writer &out) {
    const char *p = skipSpaces(args);

    if (*p == '\0') {                       // bare `aicam` → quick status
        query("ID?", out);
        query("MODEL?", out);
        return;
    }
    if (tokIs(p, "help"))    { usage(out); return; }
    if (tokIs(p, "info"))    { query("ID?", out); query("NAME?", out); query("VER?", out); return; }
    if (tokIs(p, "models"))  { query("MODELS?", out, 2500); return; }
    if (tokIs(p, "sensors") || tokIs(p, "sensor")) { query("SENSORS?", out, 2500); return; }
    if (tokIs(p, "reset"))   { _client.writeAt("RST"); out.writeln("resetting..."); return; }

    if (tokIs(p, "model")) {
        const char *a = nextTok(p);
        if (*a == '\0') { query("MODEL?", out); return; }
        char body[24];
        snprintf(body, sizeof(body), "MODEL=%d", atoi(a));
        query(body, out, 3000);
        return;
    }
    if (tokIs(p, "score") || tokIs(p, "iou")) {
        const char *a = nextTok(p);
        if (*a == '\0') { out.writeln("usage: aicam score|iou <0-100>"); return; }
        char body[24];
        snprintf(body, sizeof(body), "%s=%d", tokIs(p, "score") ? "TSCORE" : "TIOU", atoi(a));
        query(body, out);
        return;
    }
    if (tokIs(p, "invoke")) {
        const char *a = nextTok(p);
        int n = (*a == '\0') ? 1 : atoi(a);
        if (n < 1) n = 1;
        // One AT+INVOKE=n,0,1 makes the device run n times and emit n events; drain
        // and print each (the first has a longer wait for model warm-up).
        char body[24];
        snprintf(body, sizeof(body), "INVOKE=%d,0,1", n);
        _client.flush();
        _client.writeAt(body);
        int got = 0;
        for (int i = 0; i < n; i++) {
            AiResult res;
            if (!_client.nextEvent(res, got == 0 ? 4000 : 1500)) break;
            char line[256];
            format(res, line, sizeof(line));
            out.writeln(line);
            got++;
        }
        if (got == 0) out.writeln("timeout (no model loaded?)");
        return;
    }
    if (tokIs(p, "stream")) {
        const char *a = nextTok(p);
        if (tokIs(a, "on"))       { startStream(); out.writeln("streaming (aicam stream off to stop)"); }
        else if (tokIs(a, "off")) { stopStream();  out.writeln("stopped"); }
        else out.writeln("usage: aicam stream on|off");
        return;
    }
    if (tokIs(p, "snap")) {
        const char *a = nextTok(p);
        bool dump = tokIs(a, "dump");
        static char b64[AICAM_RX_MAX];          // reused, not on the stack
        int len = 0;
        if (_client.captureImage(b64, sizeof(b64), len, 5000)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "image: %d base64 bytes%s", len,
                     len >= (int)sizeof(b64) ? " (truncated, raise AICAM_RX_MAX)" : "");
            out.writeln(msg);
            if (dump) out.writeln(b64);
        } else out.writeln("timeout (no camera / SD?)");
        return;
    }
    if (tokIs(p, "at")) {
        const char *raw = nextTok(p);
        if (*raw == '\0') { out.writeln("usage: aicam at <AT command>"); return; }
        // Strip surrounding double-quotes the user may have typed (the device would
        // otherwise see them as part of the command: `Unknown command: "AT+..."`).
        char rawbuf[160];
        size_t rl = strlen(raw);
        if (rl >= 2 && raw[0] == '"' && raw[rl - 1] == '"') { raw++; rl -= 2; }
        if (rl >= sizeof(rawbuf)) rl = sizeof(rawbuf) - 1;
        memcpy(rawbuf, raw, rl);
        rawbuf[rl] = '\0';
        _client.flush();
        _client.writeRaw(rawbuf);
        // Print whatever response frames arrive: wait up to 1.5 s for the first,
        // then drain any that follow quickly.
        char frame[AICAM_FRAME_MAX];
        bool any = false;
        while (_client.readFrame(frame, sizeof(frame), any ? 400 : 1500)) {
            out.writeln(frame);
            any = true;
        }
        if (!any) out.writeln("(no response)");
        return;
    }
    out.write("unknown: "); out.writeln(p);
    usage(out);
}
