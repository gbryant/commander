#include "../hal.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include <string.h>

// The original ESP32 (and S2) have no USB-Serial/JTAG peripheral — its console is
// a regular UART over the board's USB-to-UART bridge. Chips that do have it (S3,
// C3, C6, H2, …) use the native USB CDC. SOC_USB_SERIAL_JTAG_SUPPORTED is 0 /
// undefined on chips without the peripheral, so `#if` selects the right backend.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#ifndef HAL_UART_PORT
#define HAL_UART_PORT UART_NUM_0   // UART0 = the console pins on the USB bridge
#endif
#endif

static const char *TAG = "hal_i2c";

static i2c_master_bus_handle_t _bus;
static uint32_t                _bus_speed_hz;

static i2c_master_dev_handle_t open_device(uint8_t addr) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = _bus_speed_hz;
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t ret = i2c_master_bus_add_device(_bus, &cfg, &dev);
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "open 0x%02X: %s", addr, esp_err_to_name(ret));
    return dev;
}

void hal_i2c_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t speed_hz) {
    _bus_speed_hz = speed_hz;
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port      = I2C_NUM_0;
    cfg.sda_io_num    = (gpio_num_t)sda_pin;
    cfg.scl_io_num    = (gpio_num_t)scl_pin;
    cfg.clk_source    = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_new_master_bus(&cfg, &_bus);
}

bool hal_i2c_probe(uint8_t addr) {
    return i2c_master_probe(_bus, addr, 200) == ESP_OK;
}

bool hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    i2c_master_dev_handle_t dev = open_device(addr);
    esp_err_t ret = i2c_master_transmit(dev, buf, len + 1, 50);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "write 0x%02X reg 0x%02X: %s", addr, reg, esp_err_to_name(ret));
    i2c_master_bus_rm_device(dev);
    return ret == ESP_OK;
}

bool hal_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    i2c_master_dev_handle_t dev = open_device(addr);
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, data, len, 50);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "read 0x%02X reg 0x%02X: %s", addr, reg, esp_err_to_name(ret));
    i2c_master_bus_rm_device(dev);
    return ret == ESP_OK;
}

bool hal_i2c_read_raw(uint8_t addr, uint8_t *data, size_t len) {
    i2c_master_dev_handle_t dev = open_device(addr);
    esp_err_t ret = i2c_master_receive(dev, data, len, 50);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "read_raw 0x%02X: %s", addr, esp_err_to_name(ret));
    i2c_master_bus_rm_device(dev);
    return ret == ESP_OK;
}

bool hal_i2c_write_read(uint8_t addr, const uint8_t *wdata, size_t wlen,
                        uint8_t *rdata, size_t rlen) {
    i2c_master_dev_handle_t dev = open_device(addr);
    esp_err_t ret = rlen ? i2c_master_transmit_receive(dev, wdata, wlen, rdata, rlen, 50)
                         : i2c_master_transmit(dev, wdata, wlen, 50);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "write_read 0x%02X: %s", addr, esp_err_to_name(ret));
    i2c_master_bus_rm_device(dev);
    return ret == ESP_OK;
}

// gpio_set_direction (not gpio_config) so repeated direction flips — e.g. a
// bit-banged 3-wire bus like the DS1302, which switches its IO line in/out many
// times per read — don't re-run gpio_config's pin reservation and spam IDF's
// "conflict found for GPIO[n]" warning. gpio_set_direction still routes the pin
// to the GPIO function and enables the requested direction.
void hal_gpio_set_output(uint8_t pin) { gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT); }
void hal_gpio_set_input (uint8_t pin) { gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT); }
void hal_gpio_write(uint8_t pin, bool high) { gpio_set_level((gpio_num_t)pin, high ? 1 : 0); }
bool hal_gpio_read (uint8_t pin) { return gpio_get_level((gpio_num_t)pin) != 0; }

uint32_t hal_pulse_in_us(uint8_t pin, bool level, uint32_t timeout_us) {
    int64_t t0 = esp_timer_get_time();
    while ((bool)gpio_get_level((gpio_num_t)pin) != level) {
        if (esp_timer_get_time() - t0 > timeout_us) return 0;
    }
    int64_t pulse_start = esp_timer_get_time();
    while ((bool)gpio_get_level((gpio_num_t)pin) == level) {
        if (esp_timer_get_time() - pulse_start > timeout_us) return 0;
    }
    return (uint32_t)(esp_timer_get_time() - pulse_start);
}

void     hal_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
uint64_t hal_time_us(void)         { return (uint64_t)esp_timer_get_time(); }

#if SOC_USB_SERIAL_JTAG_SUPPORTED

void hal_uart_init(uint32_t baud) {
    // baud is ignored — USB CDC negotiates speed with the host
    (void)baud;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    // ESP_ERR_INVALID_STATE = already installed by IDF console VFS; that's fine
    (void)err;
}

int hal_uart_getchar(uint32_t timeout_ms) {
    uint8_t c;
    return usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(timeout_ms)) == 1 ? (int)c : -1;
}

void hal_uart_putchar(char c)     { usb_serial_jtag_write_bytes(&c, 1, pdMS_TO_TICKS(10)); }
void hal_uart_puts(const char *s) { usb_serial_jtag_write_bytes(s, strlen(s), pdMS_TO_TICKS(100)); }

#else  // classic ESP32 / S2 — console over UART0

void hal_uart_init(uint32_t baud) {
    uart_config_t cfg = {};
    cfg.baud_rate  = (int)baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    // INVALID_STATE if the IDF console already installed a driver on this port; fine.
    esp_err_t err = uart_driver_install(HAL_UART_PORT, 256, 0, 0, NULL, 0);
    (void)err;
    uart_param_config(HAL_UART_PORT, &cfg);
    // UART0's default TX/RX pins are already routed to the USB bridge — no set_pin.
}

int hal_uart_getchar(uint32_t timeout_ms) {
    uint8_t c;
    return uart_read_bytes(HAL_UART_PORT, &c, 1, pdMS_TO_TICKS(timeout_ms)) == 1 ? (int)c : -1;
}

void hal_uart_putchar(char c)     { uart_write_bytes(HAL_UART_PORT, &c, 1); }
void hal_uart_puts(const char *s) { uart_write_bytes(HAL_UART_PORT, s, strlen(s)); }

#endif

// --- SPI / ADC / PWM ---------------------------------------------------------
// Not implemented on this platform yet. The modules that need them (st7796,
// gt911's bus is fine but joystick/buzzer/display are not) are gated to the
// platforms whose HAL backs them, via `platforms` in MODULE_SPECS
// (tools/cmdr/src/cmdr/cli.py) — so nothing can enable a peripheral this HAL
// can't drive. Implement these and widen that list in the same change.
void hal_spi_init(uint8_t, int8_t, int8_t, int8_t, uint32_t) {}
void hal_spi_set_speed(uint8_t, uint32_t)                    {}
void hal_spi_write(uint8_t, const uint8_t *, size_t)         {}
void hal_spi_write16(uint8_t, const uint16_t *, size_t)      {}
void hal_spi_transfer(uint8_t, const uint8_t *, uint8_t *, size_t) {}
int8_t   hal_adc_init(uint8_t)     { return -1; }
uint16_t hal_adc_read(uint8_t)     { return 0; }
uint16_t hal_adc_max (void)        { return 0; }
void hal_pwm_init(uint8_t)         {}
void hal_pwm_duty(uint8_t, uint8_t)   {}
void hal_pwm_tone(uint8_t, uint32_t)  {}
void hal_pwm_stop(uint8_t)            {}
