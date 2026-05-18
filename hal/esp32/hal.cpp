#include "../hal.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static i2c_master_bus_handle_t  _bus;
static i2c_master_dev_handle_t  _dev_cache[16];
static uint8_t                  _dev_addrs[16];
static uint8_t                  _dev_count = 0;

static i2c_master_dev_handle_t get_device(uint8_t addr) {
    for (uint8_t i = 0; i < _dev_count; i++)
        if (_dev_addrs[i] == addr) return _dev_cache[i];
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = 400000;
    i2c_master_dev_handle_t dev;
    i2c_master_bus_add_device(_bus, &cfg, &dev);
    _dev_addrs[_dev_count] = addr;
    _dev_cache[_dev_count++] = dev;
    return dev;
}

void hal_i2c_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t speed_hz) {
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port      = I2C_NUM_0;
    cfg.sda_io_num    = (gpio_num_t)sda_pin;
    cfg.scl_io_num    = (gpio_num_t)scl_pin;
    cfg.clk_source    = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_new_master_bus(&cfg, &_bus);
}

bool hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(get_device(addr), buf, len + 1, pdMS_TO_TICKS(10)) == ESP_OK;
}

bool hal_i2c_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(
        get_device(addr), &reg, 1, data, len, pdMS_TO_TICKS(10)) == ESP_OK;
}

void hal_gpio_set_output(uint8_t pin) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&cfg);
}
void hal_gpio_set_input(uint8_t pin) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode         = GPIO_MODE_INPUT;
    gpio_config(&cfg);
}
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

void hal_uart_init(uint32_t baud) {
    // baud is ignored — USB CDC negotiates speed with the host
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
