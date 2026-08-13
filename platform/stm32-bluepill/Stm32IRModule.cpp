#include "Stm32IRModule.h"
#include "modules/ir/IrEvent.h"
#include "hal/hal.h"
#include "stm32f1xx.h"

Stm32IRModule *Stm32IRModule::s_instance = nullptr;

// ── EXTI ISR handlers ────────────────────────────────────────────────────────
// All EXTI lines handled; only the one matching the configured pin bit fires.

#define HANDLE_EXTI(bit) \
    if (EXTI->PR & (1u << (bit))) { \
        EXTI->PR = (1u << (bit)); \
        if (Stm32IRModule::s_instance) Stm32IRModule::s_instance->onEdge(); \
    }

extern "C" void EXTI0_IRQHandler(void)     { HANDLE_EXTI(0) }
extern "C" void EXTI1_IRQHandler(void)     { HANDLE_EXTI(1) }
extern "C" void EXTI2_IRQHandler(void)     { HANDLE_EXTI(2) }
extern "C" void EXTI3_IRQHandler(void)     { HANDLE_EXTI(3) }
extern "C" void EXTI4_IRQHandler(void)     { HANDLE_EXTI(4) }
extern "C" void EXTI9_5_IRQHandler(void)   { for (uint8_t b = 5; b <= 9;  b++) { HANDLE_EXTI(b) } }
extern "C" void EXTI15_10_IRQHandler(void) { for (uint8_t b = 10; b <= 15; b++) { HANDLE_EXTI(b) } }

// ── ISR ──────────────────────────────────────────────────────────────────────

void Stm32IRModule::onEdge() {
    uint32_t now  = DWT->CYCCNT;
    uint32_t dcyc = now - _last_cyc;
    _last_cyc = now;
    if (!_active && !_wallMode) return;

    // 72 MHz → 72 cycles per µs. Cap at 15 ms to avoid overflow; reset decoders on long gaps.
    if (dcyc > 1080000u) {
        _nec.reset();
        _sony.reset();
        _wallMarks = 0;
        return;
    }
    uint32_t us = dcyc / 72u;

    // TSOP: idle = HIGH, mark = LOW. After rising edge: pin is HIGH → a mark just ended.
    bool wasMark = hal_gpio_read(_pin);

    if (_nec.feed(us, wasMark) == NecDecoder::CODE)
        pushEvent(_nec.code(), PROTO_NEC, 32);
    if (_sony.feed(us, wasMark) == SonyDecoder::CODE)
        pushEvent(_sony.code(), PROTO_SONY, _sony.bits());

#ifdef COMMANDER_IR_WALL
    if (wasMark) {
        if (us >= 360 && us <= 760) {
            if (++_wallMarks >= 3) { pushEvent(0xA5, PROTO_WALL, 0); _wallMarks = 0; }
        } else {
            _wallMarks = 0;
        }
    } else if (!(us >= 4700 && us <= 10000)) {
        _wallMarks = 0;
    }
#endif
}

void Stm32IRModule::pushEvent(uint32_t code, uint8_t proto, uint8_t nbits) {
    uint8_t next = (_ring.tail + 1) % RING_CAP;
    if (next == _ring.head) return;
    _ring.proto[_ring.tail] = proto;
    _ring.nbits[_ring.tail] = nbits;
    _ring.code [_ring.tail] = code;
    _ring.tail = next;
}

// ── IModule interface ─────────────────────────────────────────────────────────

void Stm32IRModule::init() {
    // Enable DWT cycle counter — also done by hal_uart_init, but safe to repeat.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    _last_cyc = DWT->CYCCNT;
}

bool Stm32IRModule::start() {
    if (_started) return true;

    uint8_t port = _pin >> 4;
    uint8_t bit  = _pin & 0xF;

    hal_gpio_set_input(_pin);   // floating input + enable GPIO clock

    // AFIO: route the selected port to the EXTI line for this pin bit.
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    uint8_t cr_idx   = bit / 4;
    uint8_t cr_shift = (bit % 4) * 4;
    AFIO->EXTICR[cr_idx] = (AFIO->EXTICR[cr_idx] & ~(0xFu << cr_shift))
                         | ((uint32_t)port << cr_shift);

    // EXTI: enable line, both edges.
    EXTI->IMR  |= (1u << bit);
    EXTI->RTSR |= (1u << bit);
    EXTI->FTSR |= (1u << bit);

    // NVIC: enable the IRQ for this pin bit.
    IRQn_Type irq;
    if      (bit <= 4) irq = (IRQn_Type)(EXTI0_IRQn + (int)bit);
    else if (bit <= 9) irq = EXTI9_5_IRQn;
    else               irq = EXTI15_10_IRQn;
    NVIC_SetPriority(irq, 8);
    NVIC_EnableIRQ(irq);

    s_instance = this;
    _last_cyc  = DWT->CYCCNT;
    _started   = true;
    return true;
}

void Stm32IRModule::registerCommands(CommandRegistry &reg) {
    reg.registerCommand(CMD("ir recv", "toggle IR receive mode (NEC/Sony)", CMD_IR_RECV,
        [](const char *, Writer &out, void *ctx) {
            auto *m = static_cast<Stm32IRModule *>(ctx);
            m->_wallMode = false;
            if (!m->_active) {
                m->start();
                m->_ring.head = m->_ring.tail;
                m->_active = true;
                out.writeln("listening... (ir recv to stop)");
            } else {
                m->_active = false;
                out.writeln("stopped.");
            }
        }, this));

#ifdef COMMANDER_IR_WALL
    reg.registerCommand(CMD("ir wall", "detect Roomba virtual wall transmissions", CMD_IR_WALL,
        [](const char *, Writer &out, void *ctx) {
            auto *m = static_cast<Stm32IRModule *>(ctx);
            m->_active = false;
            if (!m->_wallMode) {
                m->start();
                m->_ring.head = m->_ring.tail;
                m->_wallMode = true;
                out.writeln("watching for Roomba wall signals... (ir wall to stop)");
            } else {
                m->_wallMode = false;
                out.writeln("stopped.");
            }
        }, this));
#endif
}

void Stm32IRModule::tick() {
    if (!_active && !_wallMode) return;

    // A Sony frame is ended by the NEXT frame's leading mark, so the last frame of a press waits
    // for the following press — the console runs one press behind. Flush it once the carrier has
    // been quiet. (72 MHz DWT cycles → µs, wrap-safe 32-bit subtraction, as in the edge ISR.)
    if (_active) {
        uint32_t idle_us = (DWT->CYCCNT - _last_cyc) / 72u;
        if (_sony.flush(idle_us) == SonyDecoder::CODE)
            pushEvent(_sony.code(), PROTO_SONY, _sony.bits());
    }

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
