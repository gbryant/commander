// bp32_pico.c — Bluepad32 custom platform for the Pico 2 W (commander backend).
// Adapted from pico-bluetooth-bridge/src/my_platform.c, stripped of the I2C-slave
// half (the Pico is the I2C master now) and reduced to: read controllers, map to
// bp_raw_t, deliver via the registered callback.

#include "platform/pico/bp32_pico.h"

#include <pico/cyw43_arch.h>
#include <pico/stdlib.h>
#include <uni.h>

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Set CONFIG_BLUEPAD32_PLATFORM_CUSTOM in sdkconfig.h"
#endif

static bp_update_cb g_cb = NULL;
static void* g_cb_ctx = NULL;
static bp_raw_t g_sample;                  // last sample (neutral on disconnect)
static uni_hid_device_t* g_device = NULL;  // currently connected controller

void bp32_set_callback(bp_update_cb cb, void* ctx) {
    g_cb = cb;
    g_cb_ctx = ctx;
}

void bp32_forget_keys(void) {
    // Wipe all stored BT bonds, then resume scanning so a controller with a stale
    // or mismatched link key (auth fails, HCI reason 0x05) can pair fresh. The
    // _safe variants post to the BTstack context, so this is callable from the
    // shell task.
    uni_bt_del_keys_safe();
    uni_bt_start_scanning_and_autoconnect_safe();
}

static void deliver(void) {
    if (g_cb) g_cb(&g_sample, g_cb_ctx);
}

static void reset_sample(bool connected) {
    g_sample.connected = connected;
    g_sample.lx = g_sample.ly = g_sample.rx = g_sample.ry = 0;
    g_sample.lt = g_sample.rt = 0;
    g_sample.buttons = 0;
}

// Map Bluepad32 axis (-512..512) → our -512..511. (Already compatible; just clamp.)
static int16_t map_axis(int32_t v) {
    if (v < -512) v = -512;
    if (v > 511)  v = 511;
    return (int16_t)v;
}
static uint8_t map_trigger(uint16_t v) { return (uint8_t)((v > 1023 ? 1023 : v) >> 2); }

//
// Bluepad32 platform callbacks
//
static void plat_init(int argc, const char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    reset_sample(false);
    logi("commander: Bluepad32 controller backend up\n");
}

static void plat_on_init_complete(void) {
    logi("commander: scanning for controllers (paired pads auto-reconnect)\n");
    uni_bt_start_scanning_and_autoconnect_unsafe();
}

static uni_error_t plat_on_device_discovered(bd_addr_t addr, const char* name, uint16_t cod, uint8_t rssi) {
    ARG_UNUSED(addr); ARG_UNUSED(cod); ARG_UNUSED(rssi);
    logi("discovered: %s\n", name ? name : "?");
    return UNI_ERROR_SUCCESS;
}

static void plat_on_device_connected(uni_hid_device_t* d) {
    logi("controller connected: %s\n", d->name);
    g_device = d;
    reset_sample(true);
    deliver();
}

static void plat_on_device_disconnected(uni_hid_device_t* d) {
    ARG_UNUSED(d);
    logi("controller disconnected\n");
    g_device = NULL;
    reset_sample(false);
    deliver();
}

static uni_error_t plat_on_device_ready(uni_hid_device_t* d) {
    // PS4/DualShock 4 drops the link within seconds unless the host sends an
    // output report shortly after the HID channels open. Setting player LED 1
    // both satisfies that and gives visual feedback. (Kept from the reference.)
    if (d->report_parser.set_player_leds != NULL)
        d->report_parser.set_player_leds(d, 0x01);
    return UNI_ERROR_SUCCESS;
}

static void plat_on_controller_data(uni_hid_device_t* d, uni_controller_t* ctl) {
    ARG_UNUSED(d);
    if (ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) return;
    uni_gamepad_t* gp = &ctl->gamepad;

    g_sample.connected = true;
    g_sample.lx = map_axis(gp->axis_x);
    g_sample.ly = map_axis(gp->axis_y);
    g_sample.rx = map_axis(gp->axis_rx);
    g_sample.ry = map_axis(gp->axis_ry);
    g_sample.lt = map_trigger(gp->brake);
    g_sample.rt = map_trigger(gp->throttle);

    uint32_t b = 0;
    if (gp->buttons & BUTTON_A)          b |= (1u << BP_BTN_A);
    if (gp->buttons & BUTTON_B)          b |= (1u << BP_BTN_B);
    if (gp->buttons & BUTTON_X)          b |= (1u << BP_BTN_X);
    if (gp->buttons & BUTTON_Y)          b |= (1u << BP_BTN_Y);
    if (gp->dpad & DPAD_UP)              b |= (1u << BP_BTN_DPAD_UP);
    if (gp->dpad & DPAD_DOWN)            b |= (1u << BP_BTN_DPAD_DOWN);
    if (gp->dpad & DPAD_LEFT)            b |= (1u << BP_BTN_DPAD_LEFT);
    if (gp->dpad & DPAD_RIGHT)           b |= (1u << BP_BTN_DPAD_RIGHT);
    if (gp->buttons & BUTTON_SHOULDER_L) b |= (1u << BP_BTN_L1);
    if (gp->buttons & BUTTON_SHOULDER_R) b |= (1u << BP_BTN_R1);
    if (gp->buttons & BUTTON_TRIGGER_L)  b |= (1u << BP_BTN_L2);
    if (gp->buttons & BUTTON_TRIGGER_R)  b |= (1u << BP_BTN_R2);
    if (gp->misc_buttons & MISC_BUTTON_SELECT) b |= (1u << BP_BTN_SELECT);
    if (gp->misc_buttons & MISC_BUTTON_START)  b |= (1u << BP_BTN_START);
    if (gp->buttons & BUTTON_THUMB_L)    b |= (1u << BP_BTN_L3);
    if (gp->buttons & BUTTON_THUMB_R)    b |= (1u << BP_BTN_R3);
    if (gp->misc_buttons & MISC_BUTTON_SYSTEM) b |= (1u << BP_BTN_SYSTEM);
    g_sample.buttons = b;

    deliver();
}

static const uni_property_t* plat_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void plat_on_oob_event(uni_platform_oob_event_t event, void* data) {
    ARG_UNUSED(data);
    if (event == UNI_PLATFORM_OOB_BLUETOOTH_ENABLED) logi("bluetooth enabled\n");
}

static struct uni_platform* get_platform(void) {
    static struct uni_platform plat = {
        .name = "commander",
        .init = plat_init,
        .on_init_complete = plat_on_init_complete,
        .on_device_discovered = plat_on_device_discovered,
        .on_device_connected = plat_on_device_connected,
        .on_device_disconnected = plat_on_device_disconnected,
        .on_device_ready = plat_on_device_ready,
        .on_oob_event = plat_on_oob_event,
        .on_controller_data = plat_on_controller_data,
        .get_property = plat_get_property,
    };
    return &plat;
}

int bp32_init(void) {
    // CYW43 is brought up ONCE by the runner (shared by WiFi + BT), before
    // commander_setup() runs — so we do NOT call cyw43_arch_init() here. We just
    // register the platform and start Bluepad32. No btstack_run_loop_execute():
    // the FreeRTOS cyw43_arch async-context worker pumps BTstack. Returning lets
    // the rest of commander start its tasks.
    uni_platform_set_custom(get_platform());
    uni_init(0, NULL);
    return 0;
}
