#include "ZephyrIRModule.h"
#include "modules/ir/IrEvent.h"
#include <zephyr/kernel.h>

// IR input pin from the app overlay's zephyr,user node (see the header). Configured raw
// (non-inverted) so the read level maps directly to physical: a TSOP receiver idles HIGH
// and drives LOW under carrier, so "now HIGH" means a mark (LOW pulse) just ended.
static const struct gpio_dt_spec ir_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ir_gpios);
static struct gpio_callback      ir_cb;
static ZephyrIRModule           *g_ir = nullptr;

static void ir_isr(const struct device *, struct gpio_callback *, uint32_t) {
    if (g_ir) g_ir->onEdge();
}

void ZephyrIRModule::onEdge() {
    uint32_t now  = k_cycle_get_32();
    uint32_t dcyc = now - _last_cyc;                 // wrap-safe 32-bit subtraction
    _last_cyc = now;
    if (!_active) return;

    uint32_t us  = k_cyc_to_us_floor32(dcyc);
    bool endedMark = (gpio_pin_get_dt(&ir_pin) == 1);  // now HIGH => a LOW mark just ended
    if (_dec.feed(us, endedMark) == NecDecoder::CODE) {
        uint8_t n = (uint8_t)((_head + 1) % RING);
        if (n != _tail) { _ring[_head] = _dec.code(); _head = n; }  // drop if full
    }
}

bool ZephyrIRModule::start() {
    if (_started) return true;
    if (!gpio_is_ready_dt(&ir_pin)) return false;
    gpio_pin_configure_dt(&ir_pin, GPIO_INPUT);
    gpio_init_callback(&ir_cb, ir_isr, BIT(ir_pin.pin));
    gpio_add_callback(ir_pin.port, &ir_cb);
    gpio_pin_interrupt_configure_dt(&ir_pin, GPIO_INT_EDGE_BOTH);
    g_ir = this;
    _last_cyc = k_cycle_get_32();
    _started = true;
    return true;
}

void ZephyrIRModule::tick() {
    while (_tail != _head) {
        uint32_t code = _ring[_tail];
        _tail = (uint8_t)((_tail + 1) % RING);
        _last = code;
        _code_valid = true;
        if (_out) {
            char line[20];
            ir_format_event(line, code, kProtocolNec);
            _out->writeln(line);                     // one frame on the ir channel
        }
    }
}

void ZephyrIRModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("ir recv", "toggle IR receive (NEC) on the ir-gpios pin", CMD_IR_RECV,
        [](const char *, Writer &out, void *ctx) {
            auto *self = static_cast<ZephyrIRModule *>(ctx);
            if (!self->_active) {
                if (!self->start()) { out.writeln("ir: gpio not ready"); return; }
                self->_active = true;
                out.writeln("listening... (ir recv to stop)");
            } else {
                self->_active = false;
                out.writeln("stopped.");
            }
        }, this));
}
