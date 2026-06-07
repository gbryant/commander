#include "platform/esp32/Ws2812Module.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ws2812";

// RMT resolution: 10 MHz → 1 tick = 100 ns. WS2812 bit timings (T0H 0.3us /
// T0L 0.9us, T1H 0.9us / T1L 0.3us; >50 us low latches the frame).
#define WS2812_RES_HZ   10000000
#define T0H 3
#define T0L 9
#define T1H 9
#define T1L 3

void Ws2812Module::wire(uint8_t r, uint8_t g, uint8_t b, uint8_t *dst) const {
    switch (_order) {
        case RGB: dst[0]=r; dst[1]=g; dst[2]=b; break;
        case BRG: dst[0]=b; dst[1]=r; dst[2]=g; break;
        case RBG: dst[0]=r; dst[1]=b; dst[2]=g; break;
        case GBR: dst[0]=g; dst[1]=b; dst[2]=r; break;
        case BGR: dst[0]=b; dst[1]=g; dst[2]=r; break;
        case GRB: default: dst[0]=g; dst[1]=r; dst[2]=b; break;
    }
}

void Ws2812Module::init() {
    if (_count <= 0) { ESP_LOGE(TAG, "count must be > 0"); return; }
    _buf  = (uint8_t *)calloc(_count, 3);
    _scan = (uint8_t *)calloc(_count, 3);
    if (!_buf || !_scan) { ESP_LOGE(TAG, "alloc failed"); return; }

    rmt_channel_handle_t chan = nullptr;
    rmt_tx_channel_config_t chan_cfg = {};
    chan_cfg.gpio_num          = (gpio_num_t)_pin;
    chan_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    chan_cfg.resolution_hz     = WS2812_RES_HZ;
    chan_cfg.mem_block_symbols = 64;
    chan_cfg.trans_queue_depth = 4;
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &chan);
    if (err != ESP_OK) { ESP_LOGE(TAG, "rmt channel: %s", esp_err_to_name(err)); return; }

    // Bytes encoder mapping each data bit to a WS2812 high/low symbol.
    rmt_bytes_encoder_config_t enc_cfg = {};
    enc_cfg.bit0.level0 = 1; enc_cfg.bit0.duration0 = T0H;
    enc_cfg.bit0.level1 = 0; enc_cfg.bit0.duration1 = T0L;
    enc_cfg.bit1.level0 = 1; enc_cfg.bit1.duration0 = T1H;
    enc_cfg.bit1.level1 = 0; enc_cfg.bit1.duration1 = T1L;
    enc_cfg.flags.msb_first = 1;
    rmt_encoder_handle_t enc = nullptr;
    err = rmt_new_bytes_encoder(&enc_cfg, &enc);
    if (err != ESP_OK) { ESP_LOGE(TAG, "rmt encoder: %s", esp_err_to_name(err)); return; }

    err = rmt_enable(chan);
    if (err != ESP_OK) { ESP_LOGE(TAG, "rmt enable: %s", esp_err_to_name(err)); return; }

    _chan = chan;
    _enc  = enc;
    _ok   = true;
    show();   // all off
    ESP_LOGI(TAG, "%d WS2812 on GPIO%d", _count, _pin);
}

void Ws2812Module::setPixel(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (!_buf || i < 0 || i >= _count) return;
    wire(r, g, b, &_buf[i * 3]);
}

void Ws2812Module::fill(uint8_t r, uint8_t g, uint8_t b) {
    if (!_buf) return;
    for (int i = 0; i < _count; i++) wire(r, g, b, &_buf[i * 3]);
}

void Ws2812Module::show() {
    if (!_ok) return;
    // Apply global brightness into the scan buffer (keep _buf at full range).
    for (int i = 0; i < _count * 3; i++)
        _scan[i] = (uint8_t)((uint16_t)_buf[i] * _bright / 255);
    rmt_transmit_config_t tx = {};
    tx.loop_count      = 0;
    tx.flags.eot_level = 0;   // hold low after — the inter-frame gap latches
    rmt_transmit((rmt_channel_handle_t)_chan, (rmt_encoder_handle_t)_enc, _scan, _count * 3, &tx);
    rmt_tx_wait_all_done((rmt_channel_handle_t)_chan, 100);
}

// ── Shell command: `wled [sub]` ───────────────────────────────────────────────
void Ws2812Module::usage(Writer &out) {
    out.writeln("wled                    status");
    out.writeln("wled <r> <g> <b>        set all (0-255 each), then show");
    out.writeln("wled <i> <r> <g> <b>    set one pixel, then show");
    out.writeln("wled off                all off");
    out.writeln("wled bright <0-255>     global brightness");
}

void Ws2812Module::dispatch(const char *args, Writer &out) {
    while (*args == ' ') ++args;
    auto tok = [](const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    };
    auto next = [](const char *p) { while (*p && *p != ' ') ++p; while (*p == ' ') ++p; return p; };

    if (!_ok) { out.writeln("wled: not initialised (RMT bring-up failed — see boot log)"); return; }

    if (*args == '\0' || tok(args, "help")) {
        char b[48]; snprintf(b, sizeof(b), "wled: %d LEDs on GPIO%d, bright=%d", _count, _pin, _bright);
        out.writeln(b); usage(out); return;
    }
    if (tok(args, "off"))   { fill(0, 0, 0); show(); out.writeln("ok"); return; }
    if (tok(args, "bright") || tok(args, "brightness")) {
        char *e; long v = strtol(next(args), &e, 0);
        if (e == next(args)) { out.writeln("usage: wled bright <0-255>"); return; }
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        setBrightness((uint8_t)v); show(); out.writeln("ok"); return;
    }
    // Numeric forms: "<r> <g> <b>" (all) or "<i> <r> <g> <b>" (one).
    long n[4]; int cnt = 0; const char *p = args;
    while (cnt < 4) {
        char *e; long v = strtol(p, &e, 0);
        if (e == p) break;
        n[cnt++] = v; p = e; while (*p == ' ') ++p;
    }
    auto clamp = [](long v) -> uint8_t { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    if (cnt == 3) {
        fill(clamp(n[0]), clamp(n[1]), clamp(n[2])); show(); out.writeln("ok"); return;
    }
    if (cnt == 4) {
        if (n[0] < 0 || n[0] >= _count) { out.writeln("wled: pixel index out of range"); return; }
        setPixel((int)n[0], clamp(n[1]), clamp(n[2]), clamp(n[3])); show(); out.writeln("ok"); return;
    }
    out.writeln("wled: expected '<r> <g> <b>' or '<i> <r> <g> <b>'");
    usage(out);
}

void Ws2812Module::cmd(const char *args, Writer &out, void *ctx) {
    static_cast<Ws2812Module *>(ctx)->dispatch(args, out);
}

void Ws2812Module::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("wled", "WS2812 LEDs - 'wled' for usage", I2C_NONE, cmd, this));
}

// Weak default so the app need not override it.
extern "C" __attribute__((weak)) void commander_on_ws2812_ready(Ws2812Module &) {}
