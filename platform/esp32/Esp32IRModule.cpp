#include "Esp32IRModule.h"
#include "modules/ir/IrEvent.h"
#include "hal/hal.h"
#include "driver/rmt_rx.h"

// ── RMT done-callback helpers ────────────────────────────────────────────────
// These run in the RMT done-callback (normal ISR context — CONFIG_RMT_ISR_IRAM_SAFE
// is off), so they may touch flash: rmt_receive(), the NEC/Sony header decoders, and
// flash .rodata are all fair game. No IRAM_ATTR — an IR receiver doesn't need to keep
// decoding while the flash cache is disabled (OTA/NVS writes), and an IRAM-safe ISR
// could not call rmt_receive() to re-arm anyway.

void Esp32IRModule::pushEvent(uint32_t code, uint8_t proto, uint8_t nbits) {
    uint8_t next = (_ring.tail + 1) % RING_CAP;
    if (next == _ring.head) return;
    _ring.proto[_ring.tail] = proto;
    _ring.nbits[_ring.tail] = nbits;
    _ring.code [_ring.tail] = code;
    _ring.tail = next;
}

void Esp32IRModule::rearm() {
    static const rmt_receive_config_t cfg = {
        .signal_range_min_ns = 1250,       // 1.25 µs glitch filter
        .signal_range_max_ns = 12000000,   // 12 ms max — captures 9 ms NEC leader; idle beyond this ends the frame
    };
    rmt_receive((rmt_channel_handle_t)_rx_chan, _sym_buf, sizeof(_sym_buf), &cfg);
}

bool Esp32IRModule::onFrame(const void *raw) {
    const auto *edata = static_cast<const rmt_rx_done_event_data_t *>(raw);
    const rmt_symbol_word_t *syms = edata->received_symbols;
    size_t n = edata->num_symbols;

    _nec.reset();
    _sony.reset();

    for (size_t i = 0; i < n; i++) {
        uint16_t d0 = syms[i].duration0;
        uint16_t d1 = syms[i].duration1;
        bool     m0 = (syms[i].level0 == 0);   // TSOP: LOW = carrier = mark
        bool     m1 = (syms[i].level1 == 0);
        if (d0) {
            if (_nec.feed(d0, m0) == NecDecoder::CODE)
                pushEvent(_nec.code(), PROTO_NEC, 32);
            if (_sony.feed(d0, m0) == SonyDecoder::CODE)
                pushEvent(_sony.code(), PROTO_SONY, _sony.bits());
        }
        if (d1) {
            if (_nec.feed(d1, m1) == NecDecoder::CODE)
                pushEvent(_nec.code(), PROTO_NEC, 32);
            if (_sony.feed(d1, m1) == SonyDecoder::CODE)
                pushEvent(_sony.code(), PROTO_SONY, _sony.bits());
        }
    }
    // Sony emits CODE on the next frame's leader mark, which isn't in this RMT frame.
    // Flush by injecting a synthetic 2.4 ms leader mark.
    if (_sony.feed(2400, true) == SonyDecoder::CODE)
        pushEvent(_sony.code(), PROTO_SONY, _sony.bits());

#ifdef COMMANDER_IR_WALL
    // Roomba virtual wall: 3× (~550 µs mark + ~7350 µs space). Detected in a
    // separate pass — the short marks don't trigger NEC/Sony so the two don't interfere.
    {
        uint8_t wallCount = 0;
        for (size_t i = 0; i < n; i++) {
            if (syms[i].level0 == 0 && syms[i].duration0 >= 360 && syms[i].duration0 <= 760) {
                wallCount++;
            } else if (syms[i].duration0 > 0) {
                wallCount = 0;
            }
            if (syms[i].duration1 > 0 && syms[i].level1 == 1) {
                if (!(syms[i].duration1 >= 4700 && syms[i].duration1 <= 10000))
                    wallCount = 0;
            }
        }
        if (wallCount >= 3) pushEvent(0xA5, PROTO_WALL, 0);
    }
#endif

    rearm();
    return false;
}

static bool rmt_done_cb(rmt_channel_handle_t, const rmt_rx_done_event_data_t *edata, void *ctx) {
    return static_cast<Esp32IRModule *>(ctx)->onFrame(edata);
}

// ── IModule interface ─────────────────────────────────────────────────────────

void Esp32IRModule::init() {
    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num          = (gpio_num_t)_gpio;
    rx_cfg.clk_src           = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz     = 1000000;  // 1 µs per tick
    rx_cfg.mem_block_symbols = SYM_CAP;
    rmt_new_rx_channel(&rx_cfg, (rmt_channel_handle_t *)&_rx_chan);

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmt_done_cb;
    rmt_rx_register_event_callbacks((rmt_channel_handle_t)_rx_chan, &cbs, this);

    rmt_enable((rmt_channel_handle_t)_rx_chan);
    rearm();
}

void Esp32IRModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("ir recv", "toggle IR receive mode (NEC/Sony)", CMD_IR_RECV,
        [](const char *, Writer &out, void *ctx) {
            auto *m = static_cast<Esp32IRModule *>(ctx);
            m->_wallMode = false;
            m->_active   = !m->_active;
            if (m->_active) {
                m->_ring.head = m->_ring.tail;   // drain stale events
                out.writeln("listening... (ir recv to stop)");
            } else {
                out.writeln("stopped.");
            }
        }, this));

#ifdef COMMANDER_IR_WALL
    reg.registerCommand(CMD("ir wall", "detect Roomba virtual wall transmissions", CMD_IR_WALL,
        [](const char *, Writer &out, void *ctx) {
            auto *m = static_cast<Esp32IRModule *>(ctx);
            m->_active   = false;
            m->_wallMode = !m->_wallMode;
            if (m->_wallMode) {
                m->_ring.head = m->_ring.tail;
                out.writeln("watching for Roomba wall signals... (ir wall to stop)");
            } else {
                out.writeln("stopped.");
            }
        }, this));
#endif
}

void Esp32IRModule::tick() {
    if (!_active && !_wallMode) return;
    while (_ring.head != _ring.tail) {
        uint8_t  proto = _ring.proto[_ring.head];
        uint8_t  nbits = _ring.nbits[_ring.head];
        uint32_t code  = _ring.code [_ring.head];
        _ring.head = (_ring.head + 1) % RING_CAP;

        if (proto == PROTO_WALL) {
            if (_wallMode) hal_uart_puts("\r\n[Roomba] Virtual Wall (0xA5)\r\n");
            continue;
        }
        if (!_active) continue;

        _code      = code;
        _protocol  = proto;
        _available = true;

        const char *name; uint32_t addr; uint32_t cmd;
        if (proto == PROTO_NEC) {
            name = "NEC"; ir_nec_split(code, &addr, &cmd);
        } else {
            name = "Sony"; cmd = code & 0x7F; addr = code >> 7;
        }
        char buf[96];
        ir_format_event(buf, name, addr, cmd, code, nbits);
        hal_uart_puts("\r\n");
        hal_uart_puts(buf);
        hal_uart_puts("\r\n");
    }
}
