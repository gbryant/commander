#include "../hal.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

// Zephyr HAL backend. Commander rides Zephyr as "one more HAL provider": this maps
// the hal.h C interface onto Zephyr's polling drivers + kernel services, so commander
// runs on any Zephyr-supported board via devicetree, with core/modules untouched.
//
// Peripherals are addressed by devicetree handles (not pin numbers), so the pin args
// to hal_*_init are advisory — board pin/bus binding lives in the board DTS / overlay.
// On the Arduino Uno Q the console is routed to lpuart1 (the QRB bridge UART) via an
// overlay (chosen { zephyr,console = &lpuart1; }).

// --- UART (console) -------------------------------------------------------
// RX is interrupt-driven into a ring buffer: poll-mode uart_poll_in() can't keep up
// with 115200 input (the 1-byte RX register overruns between polls), so an ISR drains
// the FIFO and getchar() reads the buffer. TX stays poll_out (blocks until sent — no
// overrun risk from the MCU side). Needs CONFIG_UART_INTERRUPT_DRIVEN=y.
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
RING_BUF_DECLARE(uart_rx_rb, 512);

static void uart_isr(const struct device *dev, void *) {
    uart_irq_update(dev);                 // refresh cached IRQ status (returns void here)
    while (uart_irq_rx_ready(dev) > 0) {
        uint8_t buf[32];
        int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) break;
        ring_buf_put(&uart_rx_rb, buf, n);  // drop on overflow (bounded)
    }
}

void hal_uart_init(uint32_t /*baud*/) {
    // Baud + pins come from devicetree (current-speed). Wire up interrupt-driven RX.
    uart_irq_callback_user_data_set(uart_dev, uart_isr, nullptr);
    uart_irq_rx_enable(uart_dev);
}

int hal_uart_getchar(uint32_t timeout_ms) {
    uint8_t c;
    int64_t end = k_uptime_get() + (int64_t)timeout_ms;
    do {
        if (ring_buf_get(&uart_rx_rb, &c, 1) == 1) return (int)c;
        k_msleep(1);   // yield so lower-priority threads run (matches the HAL contract)
    } while (k_uptime_get() < end);
    return -1;
}

void hal_uart_putchar(char c)        { uart_poll_out(uart_dev, (unsigned char)c); }
void hal_uart_puts(const char *s)    { while (*s) uart_poll_out(uart_dev, (unsigned char)*s++); }

// --- Time -----------------------------------------------------------------
void     hal_delay_ms(uint32_t ms)   { k_msleep(ms); }
uint64_t hal_time_us(void)           { return (uint64_t)k_uptime_get() * 1000ULL; }  // ms-res for now

// --- GPIO -----------------------------------------------------------------
// Zephyr GPIO needs a (port device, pin) pair from devicetree, not a raw pin number.
// Stubbed for the spike (SystemModule needs no GPIO); a real impl would map pins via a
// gpio_dt_spec table or a DT-derived scheme.
void     hal_gpio_set_output(uint8_t)            {}
void     hal_gpio_set_input (uint8_t)            {}
void     hal_gpio_write     (uint8_t, bool)      {}
bool     hal_gpio_read      (uint8_t)            { return false; }
uint32_t hal_pulse_in_us(uint8_t, bool, uint32_t){ return 0; }

// --- I2C (stub for the spike) ---------------------------------------------
void hal_i2c_init (uint8_t, uint8_t, uint32_t)            {}
bool hal_i2c_probe(uint8_t)                               { return false; }
bool hal_i2c_write(uint8_t, uint8_t, const uint8_t*, size_t) { return false; }
bool hal_i2c_read (uint8_t, uint8_t, uint8_t*, size_t)       { return false; }
bool hal_i2c_read_raw(uint8_t, uint8_t*, size_t)            { return false; }
