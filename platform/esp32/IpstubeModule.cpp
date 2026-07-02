#include "platform/esp32/IpstubeModule.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"   // esp_lcd_new_panel_st7789
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ── stb_truetype: scalable text rasteriser (public domain, vendored) ──────────
// Compiled only in this TU (the module is itself gated by COMMANDER_ENABLE_IPSTUBE
// in the runner CMake). Glyph scratch allocations prefer PSRAM, falling back to
// internal RAM — keeps the (potentially large) bitmaps off the internal heap.
static void *ips_stb_malloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);     // free() handles either heap on ESP-IDF
}
#define STBTT_STATIC                              // keep stbtt_* symbols local to this TU
#define STBTT_malloc(x, u) ((void)(u), ips_stb_malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   // STBTT_STATIC exposes unused API
#include "third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

// ── IPSTube wiring (classic ESP32). Override any of these with -DIPSTUBE_... ───
#ifndef IPSTUBE_PIN_MOSI
#define IPSTUBE_PIN_MOSI 32
#endif
#ifndef IPSTUBE_PIN_SCLK
#define IPSTUBE_PIN_SCLK 33
#endif
#ifndef IPSTUBE_PIN_DC
#define IPSTUBE_PIN_DC   25
#endif
#ifndef IPSTUBE_PIN_RST
#define IPSTUBE_PIN_RST  26
#endif
// The TFT displays' backlight is TFT_ENABLE_PIN = GPIO4, an active-low,
// PWM-dimmable transistor (IPSTube dims it "inverted", CALCDIMVALUE = 255 - x).
// NOT GPIO5 — that's the WS2812 ambient LED chain (BACKLIGHTS_PIN), driven by
// the separate `ws2812` module (addressable protocol, not PWM).
#ifndef IPSTUBE_PIN_BL
#define IPSTUBE_PIN_BL    4
#endif
// Per-display chip-selects, left-to-right: index 0 = leftmost display (hours
// tens) … index 5 = rightmost (seconds ones), so the array reads HH:MM:SS. This
// is EleksTube's digit pin list {15,2,27,14,12,13} (seconds-ones … hours-tens)
// reversed. Driven manually as GPIOs (active-low) — the six displays share one
// SPI device, so the SPI peripheral sees no hardware CS (an ESP32 host has only
// 3 CS slots).
#ifndef IPSTUBE_CS_PINS
#define IPSTUBE_CS_PINS  {13, 12, 14, 27, 2, 15}
#endif
#ifndef IPSTUBE_SPI_HOST
#define IPSTUBE_SPI_HOST SPI2_HOST
#endif
#ifndef IPSTUBE_SPI_HZ
#define IPSTUBE_SPI_HZ   (40 * 1000 * 1000)
#endif
#ifndef IPSTUBE_SPI_MODE
#define IPSTUBE_SPI_MODE 3         // IPSTube ST7789 needs mode 3 (confirmed on HW)
#endif
#ifndef IPSTUBE_BL_ACTIVE_LOW
#define IPSTUBE_BL_ACTIVE_LOW 1    // IPSTube TFT_ENABLE is active-low (on = pin low)
#endif

// ── Panel tunables — runtime-adjustable too (ipstube invert/gap/swap/mirror) ───
#ifndef IPSTUBE_INVERT
#define IPSTUBE_INVERT 1           // ST7789 IPS panels usually need inversion on
#endif
#ifndef IPSTUBE_RGB_BGR
#define IPSTUBE_RGB_BGR 0          // set 1 if red and blue come out swapped
#endif
#ifndef IPSTUBE_GAP_X
#define IPSTUBE_GAP_X 52           // 135x240 ST7789 column offset
#endif
#ifndef IPSTUBE_GAP_Y
#define IPSTUBE_GAP_Y 40           // row offset
#endif
#ifndef IPSTUBE_SWAP_XY
#define IPSTUBE_SWAP_XY 0
#endif
#ifndef IPSTUBE_MIRROR_X
#define IPSTUBE_MIRROR_X 0
#endif
#ifndef IPSTUBE_MIRROR_Y
#define IPSTUBE_MIRROR_Y 0
#endif

static const char *TAG = "ipstube";

// One shared SPI device for all six displays; digit select is done in GPIO.
static esp_lcd_panel_io_handle_t s_io    = nullptr;
static esp_lcd_panel_handle_t    s_panel = nullptr;
static const int s_cs[IpstubeModule::kNumDisplays] = IPSTUBE_CS_PINS;

// Live tunable state (seeded from the defines; mutable via shell commands).
static bool s_invert = IPSTUBE_INVERT;
static int  s_gap_x  = IPSTUBE_GAP_X;
static int  s_gap_y  = IPSTUBE_GAP_Y;
static bool s_swap   = IPSTUBE_SWAP_XY;
static bool s_mir_x  = IPSTUBE_MIRROR_X;
static bool s_mir_y  = IPSTUBE_MIRROR_Y;
// These take effect only when the panel is (re)built — `ipstube spi`/`rgb`.
static int  s_spi_mode = IPSTUBE_SPI_MODE;
static int  s_spi_hz   = IPSTUBE_SPI_HZ;
static bool s_bgr      = IPSTUBE_RGB_BGR;

// esp_lcd queues color transactions and returns before they finish, so with
// manual CS we must wait for completion before raising the chip-select — else
// the data is still in flight when CS goes high and nothing reaches the panel.
static SemaphoreHandle_t s_done = nullptr;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

// Draw one rectangle and block until the SPI transfer actually completes.
static void draw_wait(int x0, int y0, int x1, int y1, const void *data) {
    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, data) == ESP_OK)
        xSemaphoreTake(s_done, portMAX_DELAY);
}

#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0

// ── Manual chip-select (active-low). select(-1) = all six, select(d) = one. ────
static void cs_select(int d) {
    for (int i = 0; i < IpstubeModule::kNumDisplays; i++)
        gpio_set_level((gpio_num_t)s_cs[i], (d < 0 || d == i) ? 0 : 1);
}
static void cs_none() {
    for (int i = 0; i < IpstubeModule::kNumDisplays; i++)
        gpio_set_level((gpio_num_t)s_cs[i], 1);
}

// Push the current orientation/inversion to all panels (commands need CS asserted).
static void apply_config() {
    if (!s_panel) return;
    cs_select(-1);
    esp_lcd_panel_invert_color(s_panel, s_invert);
    esp_lcd_panel_swap_xy(s_panel, s_swap);
    esp_lcd_panel_mirror(s_panel, s_mir_x, s_mir_y);
    esp_lcd_panel_set_gap(s_panel, s_gap_x, s_gap_y);
    cs_none();
}

// Re-run the ST7789 software init on all panels (not the SPI bus) — for iterating.
static void reinit_panels() {
    if (!s_panel) return;
    cs_select(-1);
    esp_lcd_panel_reset(s_panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    cs_none();
    apply_config();
}

// (Re)create the shared panel_io + ST7789 panel with the current SPI mode/clock
// and colour order. The SPI *bus* is created once in init() and kept.
static esp_err_t build_panels() {
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num         = (gpio_num_t)-1;            // manual CS via GPIO
    io_cfg.dc_gpio_num         = (gpio_num_t)IPSTUBE_PIN_DC;
    io_cfg.spi_mode            = s_spi_mode;
    io_cfg.pclk_hz             = s_spi_hz;
    io_cfg.trans_queue_depth   = 10;
    io_cfg.lcd_cmd_bits        = 8;
    io_cfg.lcd_param_bits      = 8;
    io_cfg.on_color_trans_done = on_color_done;             // draw_wait() blocks on this
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)IPSTUBE_SPI_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel_io: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = (gpio_num_t)IPSTUBE_PIN_RST;       // shared RST line
    pcfg.rgb_ele_order  = s_bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
    // ESP32 is little-endian and the SPI io sends raw memory bytes; tell the ST7789
    // (via its RAMCTRL endian bit) to read them LSB-first, so a natural RGB565 value
    // (0xF800 = red) lands correctly — no per-pixel byte-swapping in the app.
    pcfg.data_endian    = LCD_RGB_DATA_ENDIAN_LITTLE;
    pcfg.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(s_io, &pcfg, &s_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err)); return err; }
    return ESP_OK;
}

// Tear down and rebuild the panel with new SPI mode/clock/colour order, then re-init.
static bool rebuild() {
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = nullptr; }
    if (s_io)    { esp_lcd_panel_io_del(s_io); s_io = nullptr; }
    if (build_panels() != ESP_OK) return false;
    reinit_panels();
    return true;
}

// ── Bring-up ──────────────────────────────────────────────────────────────────
void IpstubeModule::init() {
    if (!s_done) s_done = xSemaphoreCreateBinary();

    // Chip-select GPIOs as outputs, idle high (all deselected).
    uint64_t cs_mask = 0;
    for (int i = 0; i < kNumDisplays; i++) cs_mask |= (1ULL << s_cs[i]);
    gpio_config_t cs_cfg = {};
    cs_cfg.pin_bit_mask = cs_mask;
    cs_cfg.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&cs_cfg);
    cs_none();

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = IPSTUBE_PIN_MOSI;
    bus.miso_io_num     = -1;                 // displays are write-only
    bus.sclk_io_num     = IPSTUBE_PIN_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = kWidth * kHeight * 2 + 16;
    esp_err_t err = spi_bus_initialize((spi_host_device_t)IPSTUBE_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err)); return; }

    // One panel_io for the shared bus — no hardware CS (cs = -1); we drive the
    // six enable lines ourselves. A single ST7789 panel object talks to whichever
    // displays are currently selected. Then reset + init + orientation on all six.
    if (build_panels() != ESP_OK) return;
    reinit_panels();

    // Backlight via LEDC PWM (shared line, all six dim together).
    ledc_timer_config_t lt = {};
    lt.speed_mode      = BL_LEDC_MODE;
    lt.duty_resolution = LEDC_TIMER_8_BIT;
    lt.timer_num       = BL_LEDC_TIMER;
    lt.freq_hz         = 5000;
    lt.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {};
    lc.gpio_num   = IPSTUBE_PIN_BL;
    lc.speed_mode = BL_LEDC_MODE;
    lc.channel    = BL_LEDC_CHANNEL;
    lc.timer_sel  = BL_LEDC_TIMER;
    lc.duty       = 0;
    lc.hpoint     = 0;
    ledc_channel_config(&lc);

    _ready = true;
    backlight(true);      // full brightness
    fill(kAll, 0x0000);   // clear all to black
    ESP_LOGI(TAG, "6x ST7789 up (%dx%d), manual CS — try `ipstube test`", kWidth, kHeight);
}

// ── Text engine (stb_truetype) ────────────────────────────────────────────────
static stbtt_fontinfo s_font;                 // valid only while s_font_ok
static bool           s_font_ok  = false;
static uint16_t      *s_panel_buf = nullptr;  // kWidth*kHeight RGB565 scratch
static uint16_t      *s_strip_buf = nullptr;  // stripWidth()*kHeight scratch (strip mode)

// Marquee cache: the message rasterized once (RGB565, bg included) into a band
// s_mq_w wide × s_mq_h tall, reused frame-to-frame while only the scroll offset
// changes. Keyed by a (px,fg,bg,text) signature so a new message rebuilds it.
static uint16_t      *s_mq_buf = nullptr;
static int            s_mq_w = 0, s_mq_h = 0, s_mq_y0 = 0;
static char           s_mq_key[192] = {0};
static int            mq_ensure(const char *str, const IpstubeModule::TextStyle &st);  // defined below

// Vertical-scroll cache: the message word-wrapped to one panel's width and
// rendered once into a tall column (kWidth × s_vs_h). A panel blits a kHeight
// window at a sliding vertical offset. Keyed like the marquee cache.
static uint16_t      *s_vs_buf = nullptr;
static int            s_vs_h = 0;
static char           s_vs_key[224] = {0};
static int            vs_ensure(const char *str, const IpstubeModule::TextStyle &st);  // defined below

static uint16_t *panel_buf() {
    if (!s_panel_buf)
        s_panel_buf = (uint16_t *)heap_caps_malloc(
            IpstubeModule::kWidth * IpstubeModule::kHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_panel_buf;
}
static uint16_t *strip_buf() {
    if (!s_strip_buf)
        s_strip_buf = (uint16_t *)heap_caps_malloc(
            IpstubeModule::stripWidth() * IpstubeModule::kHeight * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_strip_buf;
}

// Alpha-blend fg over bg in RGB565 (a = 0..255 glyph coverage).
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t a) {
    uint32_t ia = 255u - a;
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * ia) / 255;
    uint32_t g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * ia) / 255;
    uint32_t b = (( fg        & 0x1F) * a + ( bg        & 0x1F) * ia) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Sum of glyph advances (with kerning) over [begin,end) at `scale`, in pixels.
static float measure_range(const char *begin, const char *end, float scale) {
    float x = 0;
    for (const char *p = begin; p < end; ++p) {
        int aw, lsb;
        stbtt_GetCodepointHMetrics(&s_font, (unsigned char)*p, &aw, &lsb);
        x += aw * scale;
        if (p + 1 < end) x += scale * stbtt_GetCodepointKernAdvance(&s_font, (unsigned char)*p, (unsigned char)p[1]);
    }
    return x;
}
static float measure_px(const char *str, float scale) { return measure_range(str, str + strlen(str), scale); }

// Render one line of glyphs [begin,end) into dst (W*H RGB565, stride W): pen
// starts at penX, sits on baselineY. Composites fg over existing pixels; pixels
// outside [0,W)x[0,H) clip (so penX/baselineY may run off-canvas for scrolling).
// Composite one glyph (codepoint cp) at pen integer-x penXi (+ subpixel xshift),
// baseline baselineY. fg over existing pixels; clipped to [0,W)x[0,H).
static void composite_glyph(uint16_t *dst, int W, int H, int penXi, float xshift,
                            int baselineY, int cp, uint16_t fg, float scale) {
    int gx0, gy0, gx1, gy1;
    stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, xshift, 0, &gx0, &gy0, &gx1, &gy1);
    int gw = gx1 - gx0, gh = gy1 - gy0;
    if (gw <= 0 || gh <= 0) return;
    unsigned char *bm = (unsigned char *)ips_stb_malloc((size_t)gw * gh);
    if (!bm) return;
    stbtt_MakeCodepointBitmapSubpixel(&s_font, bm, gw, gh, gw, scale, scale, xshift, 0, cp);
    int dx0 = penXi + gx0;
    int dy0 = baselineY + gy0;
    for (int gy = 0; gy < gh; ++gy) {
        int dy = dy0 + gy;
        if (dy < 0 || dy >= H) continue;
        for (int gx = 0; gx < gw; ++gx) {
            int dx = dx0 + gx;
            if (dx < 0 || dx >= W) continue;
            uint8_t a = bm[gy * gw + gx];
            if (!a) continue;
            uint16_t *o = &dst[dy * W + dx];
            *o = blend565(*o, fg, a);
        }
    }
    free(bm);
}

static void render_line(uint16_t *dst, int W, int H, int penX, int baselineY,
                        const char *begin, const char *end, uint16_t fg, float scale,
                        bool hyphen = false) {
    float xpos = 0;
    for (const char *p = begin; p < end; ++p) {
        int cp = (unsigned char)*p;
        int aw, lsb;
        stbtt_GetCodepointHMetrics(&s_font, cp, &aw, &lsb);
        composite_glyph(dst, W, H, penX + (int)floorf(xpos), xpos - floorf(xpos), baselineY, cp, fg, scale);
        xpos += aw * scale;
        if (p + 1 < end) xpos += scale * stbtt_GetCodepointKernAdvance(&s_font, cp, (unsigned char)p[1]);
    }
    if (hyphen)   // a word broken across lines gets a trailing '-'
        composite_glyph(dst, W, H, penX + (int)floorf(xpos), xpos - floorf(xpos), baselineY, '-', fg, scale);
}

// One line, aligned about (ax,ay) per st.halign/valign — the single-line path.
static void blit_glyphs(uint16_t *dst, int W, int H, int ax, int ay,
                        const char *str, const IpstubeModule::TextStyle &st) {
    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)st.px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    const char *end = str + strlen(str);
    int box_w = (int)ceilf(measure_range(str, end, scale));
    int x0 = (st.halign == IpstubeModule::TextStyle::Center) ? ax - box_w / 2
           : (st.halign == IpstubeModule::TextStyle::Right)  ? ax - box_w : ax;
    int y0 = (st.valign == IpstubeModule::TextStyle::Middle) ? ay - st.px / 2
           : (st.valign == IpstubeModule::TextStyle::Bottom) ? ay - st.px : ay;
    render_line(dst, W, H, x0, y0 + (int)(scale * asc), str, end, st.fg, scale);
}

// Greedy word-wrap of `str` to width boxW at `scale`. Lines are returned as
// [begin,end) spans into the original string (no copies) — reusable for vertical
// scroll. A single word wider than boxW is force-placed on its own line (it will
// overflow; the height-fit search shrinks px until even that fits). Returns the
// line count (capped at maxLines).
struct WrapLine { const char *b; const char *e; bool hyphen; };

static float char_adv(int cp, float scale) {
    int aw, lsb;
    stbtt_GetCodepointHMetrics(&s_font, cp, &aw, &lsb);
    return aw * scale;
}
// Rendered width of a wrapped line — the span plus a trailing '-' if hyphenated.
static float line_width(const WrapLine &L, float scale) {
    return measure_range(L.b, L.e, scale) + (L.hyphen ? char_adv('-', scale) : 0);
}

// Greedy word-wrap of `str` to width boxW at `scale`. Lines are [begin,end) spans
// into the original string (no copies). A '\n' forces a line break (explicit
// layout). When `hyphenate`, a single word wider than boxW is split across lines
// with a trailing '-' (out.hyphen); otherwise it's force-placed whole (may clip)
// — flow uses whole-word, wrap/scroll hyphenate.
static int wrap_lines(const char *str, float scale, int boxW, WrapLine *out, int maxLines,
                      bool hyphenate) {
    float hyphenW = hyphenate ? char_adv('-', scale) : 0;
    int n = 0;
    const char *p = str;
    while (*p && n < maxLines) {
        while (*p == ' ') ++p;                                    // trim leading spaces (not \n)
        if (*p == IpstubeModule::kNoWrap) {                       // no-wrap line: emit whole segment
            const char *lb = p + 1, *le = p + 1;
            while (*le && *le != '\n') ++le;
            out[n].b = lb; out[n].e = le; out[n].hyphen = false; ++n;
            p = (*le == '\n') ? le + 1 : le;
            continue;
        }
        const char *lineB = p, *lineE = p;
        bool hy = false;
        for (;;) {
            const char *ws = lineE; while (*ws == ' ') ++ws;     // spaces before next word
            if (*ws == '\n') { p = ws + 1; break; }               // hard break → resume after \n
            if (!*ws) { p = ws; break; }                          // no more words
            const char *we = ws; while (*we && *we != ' ' && *we != '\n') ++we;
            if (measure_range(lineB, we, scale) <= boxW) {
                lineE = we;                                       // word fits
                if (!*we || *we == '\n') { p = (*we == '\n') ? we + 1 : we; break; }
            } else if (lineE == lineB && hyphenate) {             // lone over-long word → hyphenate
                const char *cut = ws + 1;                          // keep ≥ 1 char
                while (cut < we && measure_range(ws, cut + 1, scale) + hyphenW <= boxW) ++cut;
                lineE = cut; hy = true; p = cut;                  // rest continues next line
                break;
            } else if (lineE == lineB) {
                lineE = we;                                       // can't hyphenate → force whole
                if (!*we || *we == '\n') { p = (*we == '\n') ? we + 1 : we; break; }
            } else {
                p = ws; break;                                    // word starts next line
            }
        }
        out[n].b = lineB; out[n].e = lineE; out[n].hyphen = hy; ++n;
        if (p <= lineB && !hy) break;                             // no forward progress safety
    }
    return n;
}

// Largest px at which word-wrapped `str` fits boxW*boxH (all lines within width,
// stacked height within boxH). Binary search — total height grows monotonically
// with px. The auto-size for drawTextWrapped (style.px <= 0).
static int fit_wrap_px(const char *str, int boxW, int boxH) {
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    constexpr int kMaxLines = 24;
    int lo = 6, hi = boxH, best = 6;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        float scale = stbtt_ScaleForPixelHeight(&s_font, (float)mid);
        WrapLine L[kMaxLines];
        // Size for WHOLE words (no hyphenation) so a short token like "65°F" isn't
        // broken to justify a bigger font — the render still hyphenates as a fallback.
        int n = wrap_lines(str, scale, boxW, L, kMaxLines, false);
        float lineAdv = (asc - desc + gap) * scale;
        bool fits = (n < kMaxLines) && (n * lineAdv <= (float)boxH);
        for (int i = 0; fits && i < n; ++i)
            if (measure_range(L[i].b, L[i].e, scale) > boxW + 0.5f) fits = false;
        if (fits) { best = mid; lo = mid + 1; } else { hi = mid - 1; }
    }
    return best;
}

// Largest px at which `str` flows across at most `maxPanels` panels (each panel
// = one wrapped line of width boxW, height boxH). Line count grows monotonically
// with px, so binary search. The auto-size for drawTextFlow.
static int fit_flow_px(const char *str, int boxW, int boxH, int maxPanels) {
    constexpr int kMaxLines = 24;
    int lo = 6, hi = boxH, best = 6;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        float scale = stbtt_ScaleForPixelHeight(&s_font, (float)mid);
        WrapLine L[kMaxLines];
        int n = wrap_lines(str, scale, boxW, L, kMaxLines, false);   // flow = whole words
        bool fits = (n <= maxPanels);
        for (int i = 0; fits && i < n; ++i)
            if (measure_range(L[i].b, L[i].e, scale) > boxW + 0.5f) fits = false;
        if (fits) { best = mid; lo = mid + 1; } else { hi = mid - 1; }
    }
    return best;
}

// Wrapped multi-line text laid out inside box (boxX,boxY,boxW,boxH); each line is
// aligned per st.halign and the stack is placed per st.valign.
static void blit_wrapped(uint16_t *dst, int W, int H, int boxX, int boxY, int boxW, int boxH,
                         const char *str, const IpstubeModule::TextStyle &st) {
    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)st.px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    float lineAdv = (asc - desc + gap) * scale;
    int ascPx = (int)(asc * scale);
    constexpr int kMaxLines = 24;
    WrapLine L[kMaxLines];
    int n = wrap_lines(str, scale, boxW, L, kMaxLines, true);
    float totalH = n * lineAdv;
    int top = boxY + (st.valign == IpstubeModule::TextStyle::Middle ? (int)((boxH - totalH) / 2)
                    : st.valign == IpstubeModule::TextStyle::Bottom ? (int)(boxH - totalH) : 0);
    for (int i = 0; i < n; ++i) {
        int lw = (int)ceilf(line_width(L[i], scale));
        int x = boxX + (st.halign == IpstubeModule::TextStyle::Center ? (boxW - lw) / 2
                      : st.halign == IpstubeModule::TextStyle::Right  ? (boxW - lw) : 0);
        render_line(dst, W, H, x, top + (int)(i * lineAdv) + ascPx, L[i].b, L[i].e, st.fg, scale, L[i].hyphen);
    }
}

bool IpstubeModule::loadFont(const uint8_t *ttf, size_t len) {
    if (!ttf || len == 0) return false;
    int off = stbtt_GetFontOffsetForIndex(ttf, 0);
    if (off < 0) return false;
    s_font_ok = _font_ok = (stbtt_InitFont(&s_font, ttf, off) != 0);
    return _font_ok;
}

void IpstubeModule::measureText(const char *str, const TextStyle &s, int *w, int *h) {
    if (w) *w = (s_font_ok && str) ? (int)ceilf(measure_px(str, stbtt_ScaleForPixelHeight(&s_font, (float)s.px))) : 0;
    if (h) *h = s.px;
}

int IpstubeModule::fitPx(const char *str, int boxW, int boxH, int maxPx) const {
    if (!s_font_ok || !str || !*str || boxW <= 0 || boxH <= 0) return 0;
    constexpr int kRef = 100;                     // probe size; width is linear in px
    float w = measure_px(str, stbtt_ScaleForPixelHeight(&s_font, (float)kRef));
    int px = (w > 0.5f) ? (int)((float)boxW * kRef / w) : maxPx;   // width-limited px
    if (px > boxH)  px = boxH;                     // height-limited
    if (px > maxPx) px = maxPx;
    return px < 1 ? 1 : px;
}

bool IpstubeModule::drawTextFit(uint8_t display, const char *str, TextStyle s, int pad) {
    if (!_ready || !s_font_ok || !str) return false;
    s.px     = fitPx(str, kWidth - 2 * pad, kHeight - 2 * pad);
    s.halign = TextStyle::Center;
    s.valign = TextStyle::Middle;
    return drawText(display, str, s);
}

bool IpstubeModule::drawTextWrapped(uint8_t display, const char *str, TextStyle s, int pad) {
    if (!_ready || !s_font_ok || !str) return false;
    if (display != kAll && display >= kNumDisplays) return false;
    int boxW = kWidth - 2 * pad, boxH = kHeight - 2 * pad;
    if (boxW <= 0 || boxH <= 0) return false;
    if (s.px <= 0) s.px = fit_wrap_px(str, boxW, boxH);   // auto-size to the panel
    uint16_t *buf = panel_buf();
    if (!buf) return false;
    for (int i = 0; i < kWidth * kHeight; ++i) buf[i] = s.bg;
    blit_wrapped(buf, kWidth, kHeight, pad, pad, boxW, boxH, str, s);
    return drawBitmap(display, buf);
}

int IpstubeModule::drawTextFlow(const char *str, TextStyle s, int pad) {
    if (!_ready || !s_font_ok || !str) return 0;
    int boxW = kWidth - 2 * pad, boxH = kHeight - 2 * pad;
    if (boxW <= 0 || boxH <= 0) return 0;
    if (s.px <= 0) s.px = fit_flow_px(str, boxW, boxH, kNumDisplays);
    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)s.px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    int ascPx = (int)(asc * scale);
    WrapLine L[kNumDisplays];
    int n = wrap_lines(str, scale, boxW, L, kNumDisplays, false);   // one whole-word line per panel
    uint16_t *buf = panel_buf();
    if (!buf) return 0;
    // Each panel: clear to bg, then (if it has a line) render it, vertically
    // placed per valign and aligned per halign within the panel's inset box.
    int top = (s.valign == TextStyle::Middle ? (boxH - s.px) / 2
             : s.valign == TextStyle::Bottom ? (boxH - s.px) : 0);
    for (int d = 0; d < kNumDisplays; ++d) {
        for (int i = 0; i < kWidth * kHeight; ++i) buf[i] = s.bg;
        if (d < n) {
            int lw = (int)ceilf(measure_range(L[d].b, L[d].e, scale));
            int x = pad + (s.halign == TextStyle::Center ? (boxW - lw) / 2
                         : s.halign == TextStyle::Right  ? (boxW - lw) : 0);
            render_line(buf, kWidth, kHeight, x, pad + top + ascPx, L[d].b, L[d].e, s.fg, scale);
        }
        drawBitmap((uint8_t)d, buf);
    }
    return n;
}

bool IpstubeModule::drawText(uint8_t d, int ax, int ay, const char *str, const TextStyle &s) {
    if (!_ready || !s_font_ok || !str) return false;
    if (d != kAll && d >= kNumDisplays) return false;
    uint16_t *buf = panel_buf();
    if (!buf) return false;
    for (int i = 0; i < kWidth * kHeight; ++i) buf[i] = s.bg;
    blit_glyphs(buf, kWidth, kHeight, ax, ay, str, s);
    return drawBitmap(d, buf);
}

void IpstubeModule::drawTextStrip(int ax, int ay, const char *str, const TextStyle &s) {
    if (!_ready || !s_font_ok || !str) return;
    uint16_t *strip = strip_buf();
    uint16_t *panel = panel_buf();
    if (!strip || !panel) return;
    const int W = stripWidth();
    for (int i = 0; i < W * kHeight; ++i) strip[i] = s.bg;
    blit_glyphs(strip, W, kHeight, ax, ay, str, s);
    // esp_lcd needs packed w*h data, so copy each panel's column window out.
    for (int d = 0; d < kNumDisplays; ++d) {
        int col = d * kWidth;
        for (int y = 0; y < kHeight; ++y)
            memcpy(&panel[y * kWidth], &strip[y * W + col], kWidth * 2);
        drawBitmap((uint8_t)d, panel);
    }
}

void IpstubeModule::drawMarquee(int ax, const char *str, const TextStyle &s) {
    if (!_ready || !s_font_ok || !str) return;
    int rebuilt = mq_ensure(str, s);
    if (rebuilt < 0) return;
    if (rebuilt) fill(kAll, s.bg);          // new message — black out the full panels once
    uint16_t *panel = panel_buf();
    if (!panel) return;
    // No rasterizing here: for each panel, window the cached message band at the
    // current scroll offset (bg outside the text), and blit just those band rows.
    for (int d = 0; d < kNumDisplays; ++d) {
        int stripX = d * kWidth;            // strip x of this panel's left edge
        for (int ry = 0; ry < s_mq_h; ++ry) {
            uint16_t *dst = &panel[ry * kWidth];
            const uint16_t *src = &s_mq_buf[ry * s_mq_w];
            for (int lx = 0; lx < kWidth; ++lx) {
                int sx = stripX + lx - ax;  // source column in the message
                dst[lx] = (sx >= 0 && sx < s_mq_w) ? src[sx] : s.bg;
            }
        }
        drawBitmap((uint8_t)d, 0, s_mq_y0, kWidth, s_mq_h, panel);
    }
}

int IpstubeModule::vscrollHeight(const char *str, const TextStyle &s) {
    if (!_ready || !s_font_ok || !str) return 0;
    if (vs_ensure(str, s) < 0) return 0;
    return s_vs_h;                       // total column height — the ay sweep range
}

void IpstubeModule::drawVScroll(uint8_t display, int ay, const char *str, const TextStyle &s) {
    if (!_ready || !s_font_ok || !str) return;
    if (display != kAll && display >= kNumDisplays) return;
    if (vs_ensure(str, s) < 0) return;
    uint16_t *panel = panel_buf();
    if (!panel) return;
    // Window the cached column at vertical offset ay: panel row r shows column row
    // ay+r (bg where that's outside the column). Same width → contiguous memcpy.
    for (int r = 0; r < kHeight; ++r) {
        int sy = ay + r;
        uint16_t *dst = &panel[r * kWidth];
        if (sy >= 0 && sy < s_vs_h) {
            memcpy(dst, &s_vs_buf[sy * kWidth], kWidth * 2);
        } else {
            for (int x = 0; x < kWidth; ++x) dst[x] = s.bg;
        }
    }
    drawBitmap(display, panel);          // single panel = 1/6 the bytes → high fps
}

void IpstubeModule::drawHScroll(uint8_t display, int ax, const char *str, const TextStyle &s) {
    if (!_ready || !s_font_ok || !str) return;
    if (display != kAll && display >= kNumDisplays) return;
    int rebuilt = mq_ensure(str, s);     // shares the marquee band cache
    if (rebuilt < 0) return;
    if (rebuilt) fill(display, s.bg);    // new message — clear the panel(s) once (band-only blit after)
    uint16_t *panel = panel_buf();
    if (!panel) return;
    // Like drawMarquee but the message's left edge sits at local x = ax within this
    // one panel (no per-panel strip offset). Sweep ax kWidth → -<textWidth> to scroll.
    for (int ry = 0; ry < s_mq_h; ++ry) {
        uint16_t *dst = &panel[ry * kWidth];
        const uint16_t *src = &s_mq_buf[ry * s_mq_w];
        for (int lx = 0; lx < kWidth; ++lx) {
            int sx = lx - ax;
            dst[lx] = (sx >= 0 && sx < s_mq_w) ? src[sx] : s.bg;
        }
    }
    drawBitmap(display, 0, s_mq_y0, kWidth, s_mq_h, panel);
}

// Ensure the marquee cache holds `str` at `st`. Returns 1 if it rebuilt (caller
// should clear the panels to bg first), 0 on a cache hit, -1 on failure. The
// raster is just the text band (px + headroom), centered vertically — so frames
// blit ~2/3 the rows of a full panel.
static int mq_ensure(const char *str, const IpstubeModule::TextStyle &st) {
    char key[192];
    snprintf(key, sizeof(key), "%d|%04x|%04x|%s", st.px, st.fg, st.bg, str);
    if (s_mq_buf && strcmp(key, s_mq_key) == 0) return 0;          // cache hit

    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)st.px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    const char *end = str + strlen(str);
    int w = (int)ceilf(measure_range(str, end, scale));
    if (w < 1) w = 1;
    int bandH = st.px + st.px / 6;                                  // ascender/descender headroom
    if (bandH > IpstubeModule::kHeight) bandH = IpstubeModule::kHeight;

    if (s_mq_buf) { free(s_mq_buf); s_mq_buf = nullptr; }
    s_mq_buf = (uint16_t *)heap_caps_malloc((size_t)w * bandH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mq_buf) { s_mq_w = s_mq_h = 0; s_mq_key[0] = '\0'; return -1; }
    s_mq_w  = w;
    s_mq_h  = bandH;
    s_mq_y0 = (IpstubeModule::kHeight - bandH) / 2;
    for (int i = 0; i < w * bandH; ++i) s_mq_buf[i] = st.bg;
    render_line(s_mq_buf, w, bandH, 0, (bandH - st.px) / 2 + (int)(asc * scale), str, end, st.fg, scale);
    strncpy(s_mq_key, key, sizeof(s_mq_key) - 1);
    s_mq_key[sizeof(s_mq_key) - 1] = '\0';
    return 1;
}

// Ensure the vertical-scroll cache holds `str` at `st`: the message wrapped to
// one panel's width and rendered into a kWidth × s_vs_h column. Returns 1 if it
// rebuilt, 0 on a cache hit, -1 on failure. The column width equals the panel
// width, so window rows blit as a contiguous memcpy.
static int vs_ensure(const char *str, const IpstubeModule::TextStyle &st) {
    char key[224];
    snprintf(key, sizeof(key), "%d|%04x|%04x|%d|%s", st.px, st.fg, st.bg, (int)st.halign, str);
    if (s_vs_buf && strcmp(key, s_vs_key) == 0) return 0;          // cache hit

    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)st.px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    float lineAdv = (asc - desc + gap) * scale;
    const int pad = 8;
    int boxW = IpstubeModule::kWidth - 2 * pad;
    if (boxW < 1) boxW = IpstubeModule::kWidth;
    constexpr int kMaxLines = 64;
    WrapLine L[kMaxLines];
    int n = wrap_lines(str, scale, boxW, L, kMaxLines, true);   // hyphenate long words
    int H = (int)ceilf(n * lineAdv) + (int)ceilf(lineAdv * 0.3f);  // small tail past the last line
    if (H < 1) H = 1;

    if (s_vs_buf) { free(s_vs_buf); s_vs_buf = nullptr; }
    s_vs_buf = (uint16_t *)heap_caps_malloc((size_t)IpstubeModule::kWidth * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_vs_buf) { s_vs_h = 0; s_vs_key[0] = '\0'; return -1; }
    s_vs_h = H;
    for (int i = 0; i < IpstubeModule::kWidth * H; ++i) s_vs_buf[i] = st.bg;
    int ascPx = (int)(asc * scale);
    for (int i = 0; i < n; ++i) {
        int lw = (int)ceilf(line_width(L[i], scale));
        int x = pad + (st.halign == IpstubeModule::TextStyle::Center ? (boxW - lw) / 2
                     : st.halign == IpstubeModule::TextStyle::Right  ? (boxW - lw) : 0);
        render_line(s_vs_buf, IpstubeModule::kWidth, H, x, (int)(i * lineAdv) + ascPx, L[i].b, L[i].e, st.fg, scale, L[i].hyphen);
    }
    strncpy(s_vs_key, key, sizeof(s_vs_key) - 1);
    s_vs_key[sizeof(s_vs_key) - 1] = '\0';
    return 1;
}

// ── App API ───────────────────────────────────────────────────────────────────
bool IpstubeModule::drawBitmap(uint8_t d, int x, int y, int w, int h, const uint16_t *data) {
    if (!_ready || !data) return false;
    if (d != kAll && d >= kNumDisplays) return false;
    cs_select(d == kAll ? -1 : (int)d);     // kAll → same image to all six at once
    draw_wait(x, y, x + w, y + h, data);    // waits for completion before CS up
    cs_none();
    return true;
}

void IpstubeModule::fill(uint8_t d, uint16_t color) {
    if (!_ready) return;
    if (d != kAll && d >= kNumDisplays) return;

    // Solid fill in row-stripes (one transfer per ~16 rows) instead of per-line —
    // 240 rows → 15 transfers. DMA-safe internal .bss; 240 = 16*15 (even).
    static constexpr int kStripeRows = 16;
    static uint16_t stripe[kWidth * kStripeRows];
    for (int i = 0; i < kWidth * kStripeRows; i++) stripe[i] = color;

    // kAll fills every display with identical data, so assert all six CS and draw
    // once (the parallel-write trick init() uses) — 6x fewer transfers.
    cs_select(d == kAll ? -1 : (int)d);
    for (int y = 0; y < kHeight; y += kStripeRows) {
        int rows = (kHeight - y < kStripeRows) ? (kHeight - y) : kStripeRows;
        draw_wait(0, y, kWidth, y + rows, stripe);
    }
    cs_none();
}

void IpstubeModule::backlight(bool on) { setBrightness(on ? 255 : 0); }

void IpstubeModule::setBrightness(uint8_t duty) {
    if (!_ready) return;
    uint32_t d = IPSTUBE_BL_ACTIVE_LOW ? (255u - duty) : duty;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, d);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

// ── Shell command: `ipstube [sub]` ────────────────────────────────────────────
void IpstubeModule::usage(Writer &out) {
    out.writeln("ipstube                 status");
    out.writeln("ipstube info            dump pins + current tunables");
    out.writeln("ipstube on | off        backlight full / off");
    out.writeln("ipstube dim <0-255>     backlight PWM duty");
    out.writeln("ipstube fill <d|all> <rgb565>   solid colour (hex/dec)");
    out.writeln("ipstube clear <d|all>   fill black");
    out.writeln("ipstube text <d|all> <s>  draw text (needs app-loaded font)");
    out.writeln("ipstube fit <d|all> <s>   draw text sized to fill the panel");
    out.writeln("ipstube wrap <d|all> <s>  word-wrap, auto-sized to the panel");
    out.writeln("ipstube flow <s>        flow text across panels (whole words)");
    out.writeln("ipstube scroll <s>      one frame of marquee text across all six");
    out.writeln("ipstube vscroll <d|all> <s>  top frame of a vertical reader (app scrolls)");
    out.writeln("ipstube hscroll <d|all> <s>  start frame of a 1-panel scroller (app scrolls)");
    out.writeln("ipstube test            R G B Y C W bars, one per display");
    out.writeln("ipstube cs <d|all|none> drive chip-selects (wiring probe)");
    out.writeln("ipstube reinit          re-run ST7789 init on all panels");
    out.writeln("ipstube invert <0|1>    colour inversion");
    out.writeln("ipstube swap <0|1>      swap X/Y (rotate)");
    out.writeln("ipstube mirror <x> <y>  mirror axes (0/1 each)");
    out.writeln("ipstube gap <x> <y>     panel offset (re-fill to see)");
    out.writeln("ipstube spi <mode> [hz] rebuild bus: SPI mode 0-3, opt clock Hz");
    out.writeln("ipstube rgb <0|1>       colour order RGB(0)/BGR(1), rebuilds");
}

void IpstubeModule::dispatch(const char *args, Writer &out) {
    while (*args == ' ') ++args;
    auto tok = [](const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    };
    auto next = [](const char *p) { while (*p && *p != ' ') ++p; while (*p == ' ') ++p; return p; };
    auto num  = [](const char *p, long *v) { char *e; *v = strtol(p, &e, 0); return e != p; };
    auto digit = [&](const char *p, uint8_t *out_d) -> bool {
        if (tok(p, "all")) { *out_d = kAll; return true; }
        long v; if (!num(p, &v) || v < 0 || v >= kNumDisplays) return false;
        *out_d = (uint8_t)v; return true;
    };

    if (!_ready) { out.writeln("ipstube: not initialised (SPI/panel bring-up failed — see boot log)"); return; }

    if (*args == '\0' || tok(args, "help")) { usage(out); return; }

    if (tok(args, "info")) {
        char b[96];
        snprintf(b, sizeof(b), "spi: host=%d mosi=%d sclk=%d dc=%d rst=%d hz=%d mode=%d",
                 (int)IPSTUBE_SPI_HOST, IPSTUBE_PIN_MOSI, IPSTUBE_PIN_SCLK,
                 IPSTUBE_PIN_DC, IPSTUBE_PIN_RST, s_spi_hz, s_spi_mode);
        out.writeln(b);
        snprintf(b, sizeof(b), "cs:  %d %d %d %d %d %d",
                 s_cs[0], s_cs[1], s_cs[2], s_cs[3], s_cs[4], s_cs[5]);
        out.writeln(b);
        snprintf(b, sizeof(b), "bl:  pin=%d active_low=%d", IPSTUBE_PIN_BL, IPSTUBE_BL_ACTIVE_LOW);
        out.writeln(b);
        snprintf(b, sizeof(b), "cfg: invert=%d bgr=%d swap=%d mirror=%d,%d gap=%d,%d",
                 s_invert, s_bgr, s_swap, s_mir_x, s_mir_y, s_gap_x, s_gap_y);
        out.writeln(b);
        snprintf(b, sizeof(b), "font: %s", s_font_ok ? "loaded" : "none (app must loadFont)");
        out.writeln(b);
        return;
    }
    if (tok(args, "on"))   { backlight(true);  out.writeln("ok"); return; }
    if (tok(args, "off"))  { backlight(false); out.writeln("ok"); return; }
    if (tok(args, "dim") || tok(args, "bl")) {
        long v; if (!num(next(args), &v)) { out.writeln("usage: ipstube dim <0-255>"); return; }
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        setBrightness((uint8_t)v); out.writeln("ok"); return;
    }
    if (tok(args, "clear")) {
        uint8_t d = kAll; const char *p = next(args);
        if (*p && !digit(p, &d)) { out.writeln("usage: ipstube clear <d|all>"); return; }
        fill(d, 0x0000); out.writeln("ok"); return;
    }
    if (tok(args, "fill")) {
        const char *p = next(args); uint8_t d;
        if (!digit(p, &d)) { out.writeln("usage: ipstube fill <d|all> <rgb565>"); return; }
        long v; if (!num(next(p), &v)) { out.writeln("usage: ipstube fill <d|all> <rgb565>"); return; }
        fill(d, (uint16_t)v); out.writeln("ok"); return;
    }
    if (tok(args, "text")) {
        const char *p = next(args); uint8_t d;
        if (!digit(p, &d)) { out.writeln("usage: ipstube text <d|all> <string>"); return; }
        const char *str = next(p);
        if (*str == '\0') { out.writeln("usage: ipstube text <d|all> <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st;     // defaults: 120px white-on-black, centred
        out.writeln(drawText(d, str, st) ? "ok" : "err");
        return;
    }
    if (tok(args, "fit")) {
        const char *p = next(args); uint8_t d;
        if (!digit(p, &d)) { out.writeln("usage: ipstube fit <d|all> <string>"); return; }
        const char *str = next(p);
        if (*str == '\0') { out.writeln("usage: ipstube fit <d|all> <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st;
        out.writeln(drawTextFit(d, str, st) ? "ok" : "err");
        return;
    }
    if (tok(args, "wrap")) {
        const char *p = next(args); uint8_t d;
        if (!digit(p, &d)) { out.writeln("usage: ipstube wrap <d|all> <string>"); return; }
        const char *str = next(p);
        if (*str == '\0') { out.writeln("usage: ipstube wrap <d|all> <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st; st.px = 0;     // auto-size to fill the panel
        out.writeln(drawTextWrapped(d, str, st) ? "ok" : "err");
        return;
    }
    if (tok(args, "vscroll")) {
        const char *p = next(args); uint8_t d;
        if (!digit(p, &d)) { out.writeln("usage: ipstube vscroll <d|all> <string>"); return; }
        const char *str = next(p);
        if (*str == '\0') { out.writeln("usage: ipstube vscroll <d|all> <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st; st.px = 40; st.halign = TextStyle::Left;   // a reading size
        drawVScroll(d, 0, str, st);     // ay=0 → top of the wrapped column
        out.writeln("ok (top frame — the app sweeps ay to scroll)");
        return;
    }
    if (tok(args, "hscroll")) {
        const char *p = next(args); uint8_t d;
        if (!digit(p, &d)) { out.writeln("usage: ipstube hscroll <d|all> <string>"); return; }
        const char *str = next(p);
        if (*str == '\0') { out.writeln("usage: ipstube hscroll <d|all> <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st;                   // default 120px white-on-black
        drawHScroll(d, 0, str, st);     // ax=0 → message start at the panel's left edge
        out.writeln("ok (start frame — the app sweeps ax to scroll)");
        return;
    }
    if (tok(args, "flow")) {
        const char *str = next(args);
        if (*str == '\0') { out.writeln("usage: ipstube flow <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st; st.px = 0;     // auto-size to flow across as few panels as possible
        int used = drawTextFlow(str, st);
        char b[40]; snprintf(b, sizeof(b), "ok (%d panel%s)", used, used == 1 ? "" : "s");
        out.writeln(b);
        return;
    }
    if (tok(args, "scroll")) {
        const char *str = next(args);
        if (*str == '\0') { out.writeln("usage: ipstube scroll <string>"); return; }
        if (!s_font_ok)   { out.writeln("ipstube: no font loaded (app must call loadFont)"); return; }
        TextStyle st;     // one static frame, left-anchored near the strip start
        st.halign = TextStyle::Left;
        drawTextStrip(8, kHeight / 2, str, st);
        out.writeln("ok (one frame — the app drives the animation)");
        return;
    }
    if (tok(args, "test")) {
        static const uint16_t bars[kNumDisplays] =
            {0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xFFFF};  // R G B Y C W
        for (int i = 0; i < kNumDisplays; i++) fill((uint8_t)i, bars[i]);
        out.writeln("ok"); return;
    }
    if (tok(args, "cs")) {
        const char *p = next(args);
        if (tok(p, "none")) { cs_none(); out.writeln("cs: all deselected"); return; }
        uint8_t d; if (!digit(p, &d)) { out.writeln("usage: ipstube cs <d|all|none>"); return; }
        cs_select(d == kAll ? -1 : (int)d);
        out.writeln("cs: asserted (low) — next draw command re-drives CS");
        return;
    }
    if (tok(args, "reinit")) { reinit_panels(); fill(kAll, 0x0000); out.writeln("ok: reinitialised"); return; }
    if (tok(args, "invert")) {
        long v; if (!num(next(args), &v)) { out.writeln("usage: ipstube invert <0|1>"); return; }
        s_invert = v != 0; apply_config(); out.writeln("ok"); return;
    }
    if (tok(args, "swap")) {
        long v; if (!num(next(args), &v)) { out.writeln("usage: ipstube swap <0|1>"); return; }
        s_swap = v != 0; apply_config(); out.writeln("ok (re-fill to see)"); return;
    }
    if (tok(args, "mirror")) {
        const char *p = next(args); long x, y;
        if (!num(p, &x) || !num(next(p), &y)) { out.writeln("usage: ipstube mirror <x> <y>"); return; }
        s_mir_x = x != 0; s_mir_y = y != 0; apply_config(); out.writeln("ok (re-fill to see)"); return;
    }
    if (tok(args, "gap")) {
        const char *p = next(args); long x, y;
        if (!num(p, &x) || !num(next(p), &y)) { out.writeln("usage: ipstube gap <x> <y>"); return; }
        s_gap_x = (int)x; s_gap_y = (int)y; apply_config(); out.writeln("ok (re-fill to see)"); return;
    }
    if (tok(args, "spi")) {
        const char *p = next(args); long mode, hz;
        if (!num(p, &mode) || mode < 0 || mode > 3) { out.writeln("usage: ipstube spi <mode 0-3> [hz]"); return; }
        s_spi_mode = (int)mode;
        if (num(next(p), &hz) && hz > 0) s_spi_hz = (int)hz;
        out.writeln(rebuild() ? "ok: rebuilt (re-fill to see)" : "err: rebuild failed (see log)");
        return;
    }
    if (tok(args, "rgb")) {
        long v; if (!num(next(args), &v)) { out.writeln("usage: ipstube rgb <0|1>"); return; }
        s_bgr = v != 0;
        out.writeln(rebuild() ? "ok: rebuilt (re-fill to see)" : "err: rebuild failed (see log)");
        return;
    }
    out.write("unknown ipstube subcommand: "); out.writeln(args);
    usage(out);
}

void IpstubeModule::cmd(const char *args, Writer &out, void *ctx) {
    static_cast<IpstubeModule *>(ctx)->dispatch(args, out);
}

void IpstubeModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("ipstube", "6x ST7789 displays - 'ipstube' for usage",
                            I2C_NONE, cmd, this));
}

// Weak default so the app need not override it (e.g. for pure bring-up testing).
extern "C" __attribute__((weak)) void commander_on_ipstube_ready(IpstubeModule &) {}
