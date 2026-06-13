#include "ChannelBusRunner.h"
#include "hal/hal.h"

// Byte writer for the bus: frames carry 0x00 delimiters, so we must push raw bytes —
// hal_uart_puts would stop at the first NUL. Loops hal_uart_putchar over the frame.
static void busWrite(const uint8_t *data, size_t len, void *) {
    for (size_t i = 0; i < len; i++) hal_uart_putchar((char)data[i]);
}

void ChannelBusRunner::wire(CommandRegistry &reg) {
    _ct.begin(reg, busWrite, nullptr);
    // Hand the app the begun transport so it can publisher()/subscribe() (e.g. wire an
    // IR module to publish on an `ir` channel). Weak: unset on non-bus apps -> skipped.
    if (commander_on_channels_ready) commander_on_channels_ready(_ct);
}

void ChannelBusRunner::begin(CommandRegistry &reg, uint32_t baud) {
    hal_uart_init(baud);
    wire(reg);
}

void ChannelBusRunner::begin(CommandRegistry &reg) {
    wire(reg);
}

void ChannelBusRunner::addTicker(IModule &m) {
    if (_tickCount < kMaxTickers) _tickers[_tickCount++] = &m;
}

void ChannelBusRunner::taskBody(void *self) {
    auto *r = static_cast<ChannelBusRunner *>(self);
    hal_delay_ms(1500);  // let USB CDC / the host link settle before framing output
    for (;;) {
        int c = hal_uart_getchar(10);  // 10 ms poll; HAL yields on blocking-UART platforms
        if (c >= 0) r->_ct.feedByte((uint8_t)c);
        for (uint8_t i = 0; i < r->_tickCount; i++) r->_tickers[i]->tick();
    }
}
