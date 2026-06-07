#include "platform/esp32/IpstubeModule.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"   // esp_lcd_new_panel_st7789
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

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
#ifndef IPSTUBE_PIN_BL
#define IPSTUBE_PIN_BL    5
#endif
// Per-display chip-selects, digit order: seconds-ones … hours-tens. Driven
// manually as GPIOs (active-low) — the six displays share one SPI device, so
// the SPI peripheral sees no hardware CS (an ESP32 host has only 3 CS slots).
#ifndef IPSTUBE_CS_PINS
#define IPSTUBE_CS_PINS  {15, 2, 27, 14, 12, 13}
#endif
#ifndef IPSTUBE_SPI_HOST
#define IPSTUBE_SPI_HOST SPI2_HOST
#endif
#ifndef IPSTUBE_SPI_HZ
#define IPSTUBE_SPI_HZ   (40 * 1000 * 1000)
#endif

// ── Panel tunables — adjust if the picture is offset / inverted / mirrored ─────
#ifndef IPSTUBE_INVERT
#define IPSTUBE_INVERT 1            // ST7789 IPS panels usually need inversion on
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

// ── Bring-up ──────────────────────────────────────────────────────────────────
void IpstubeModule::init() {
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
    // six enable lines ourselves. So a single ST7789 panel object talks to
    // whichever displays are currently selected.
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = (gpio_num_t)-1;
    io_cfg.dc_gpio_num       = (gpio_num_t)IPSTUBE_PIN_DC;
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = IPSTUBE_SPI_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)IPSTUBE_SPI_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel_io: %s", esp_err_to_name(err)); return; }

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = (gpio_num_t)IPSTUBE_PIN_RST;   // shared RST line
    pcfg.rgb_ele_order  = IPSTUBE_RGB_BGR ? LCD_RGB_ELEMENT_ORDER_BGR
                                          : LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.bits_per_pixel = 16;
    err = esp_lcd_new_panel_st7789(s_io, &pcfg, &s_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err)); return; }

    // Initialise all six at once: select them all, then reset + init goes to
    // every display in parallel over the shared bus.
    cs_select(-1);
    esp_lcd_panel_reset(s_panel);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_init(s_panel);
#if IPSTUBE_INVERT
    esp_lcd_panel_invert_color(s_panel, true);
#endif
    esp_lcd_panel_set_gap(s_panel, IPSTUBE_GAP_X, IPSTUBE_GAP_Y);
    esp_lcd_panel_mirror(s_panel, IPSTUBE_MIRROR_X, IPSTUBE_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);
    cs_none();

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
    lc.duty       = 255;
    lc.hpoint     = 0;
    ledc_channel_config(&lc);

    _ready = true;
    fill(kAll, 0x0000);   // clear all to black
    ESP_LOGI(TAG, "6x ST7789 up (%dx%d) on SPI%d, manual CS", kWidth, kHeight, (int)IPSTUBE_SPI_HOST);
}

// ── App API ───────────────────────────────────────────────────────────────────
bool IpstubeModule::pushDigit(uint8_t d, const uint16_t *fb) {
    if (!_ready || d >= kNumDisplays || !fb) return false;
    cs_select(d);
    // draw_bitmap is blocking here (no on_color_trans_done callback registered),
    // so the CS line is safe to raise once it returns. x_end/y_end are exclusive.
    esp_err_t r = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, kWidth, kHeight, fb);
    cs_none();
    return r == ESP_OK;
}

void IpstubeModule::fill(uint8_t d, uint16_t color) {
    if (!_ready) return;
    static uint16_t line[kWidth];
    for (int x = 0; x < kWidth; x++) line[x] = color;
    for (int i = 0; i < kNumDisplays; i++) {
        if (d != kAll && d != i) continue;
        cs_select(i);
        for (int y = 0; y < kHeight; y++)
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, kWidth, y + 1, line);
        cs_none();
    }
}

void IpstubeModule::backlight(bool on) { setBrightness(on ? 255 : 0); }

void IpstubeModule::setBrightness(uint8_t duty) {
    if (!_ready) return;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

// ── Shell command: `ipstube [sub]` ────────────────────────────────────────────
void IpstubeModule::usage(Writer &out) {
    out.writeln("ipstube                 status");
    out.writeln("ipstube on | off        backlight full / off");
    out.writeln("ipstube dim <0-255>     backlight PWM duty");
    out.writeln("ipstube fill <d|all> <rgb565>   solid colour (rgb565 hex/dec)");
    out.writeln("ipstube clear <d|all>   fill black");
    out.writeln("ipstube test            colour bars across all six");
}

void IpstubeModule::dispatch(const char *args, Writer &out) {
    while (*args == ' ') ++args;
    auto tok = [](const char *p, const char *t) {
        size_t n = strlen(t);
        return strncmp(p, t, n) == 0 && (p[n] == '\0' || p[n] == ' ');
    };
    auto next = [](const char *p) { while (*p && *p != ' ') ++p; while (*p == ' ') ++p; return p; };
    auto digit = [&](const char *p, uint8_t *out_d) -> bool {
        if (tok(p, "all")) { *out_d = kAll; return true; }
        char *e; long v = strtol(p, &e, 0);
        if (e == p || v < 0 || v >= kNumDisplays) return false;
        *out_d = (uint8_t)v; return true;
    };

    if (!_ready) { out.writeln("ipstube: not initialised (SPI/panel bring-up failed — see boot log)"); return; }

    if (*args == '\0') {
        char b[16]; snprintf(b, sizeof(b), "%dx%d", kWidth, kHeight);
        out.write("ipstube: 6x ST7789 "); out.write(b); out.writeln(" ready");
        return;
    }
    if (tok(args, "help")) { usage(out); return; }
    if (tok(args, "on"))   { backlight(true);  out.writeln("ok"); return; }
    if (tok(args, "off"))  { backlight(false); out.writeln("ok"); return; }
    if (tok(args, "dim")) {
        const char *p = next(args);
        char *e; long v = strtol(p, &e, 0);
        if (e == p) { out.writeln("usage: ipstube dim <0-255>"); return; }
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
        const char *c = next(p);
        char *e; long v = strtol(c, &e, 0);
        if (e == c) { out.writeln("usage: ipstube fill <d|all> <rgb565>"); return; }
        fill(d, (uint16_t)v); out.writeln("ok"); return;
    }
    if (tok(args, "test")) {
        static const uint16_t bars[kNumDisplays] =
            {0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xFFFF};  // R G B Y C W
        for (int i = 0; i < kNumDisplays; i++) fill((uint8_t)i, bars[i]);
        out.writeln("ok"); return;
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
