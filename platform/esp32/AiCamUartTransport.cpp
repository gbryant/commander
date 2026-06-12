#include "platform/esp32/AiCamUartTransport.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

// Pins / port / buffers are overridable so a different wiring or UART can be used.
#ifndef AICAM_UART_PORT
#define AICAM_UART_PORT UART_NUM_1
#endif
#ifndef AICAM_UART_RX_BUF
// A whole SAMPLE (base64 JPEG) can arrive in a burst; a roomy ring avoids overruns
// between pump() calls at 921600 baud.
#define AICAM_UART_RX_BUF 8192
#endif

static const char *TAG = "aicam_uart";

void AiCamUartTransport::begin() {
    if (_ready) return;

    uart_config_t cfg = {};
    cfg.baud_rate  = (int)_baud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(AICAM_UART_PORT, AICAM_UART_RX_BUF, 0, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(err));
        return;
    }
    uart_param_config(AICAM_UART_PORT, &cfg);
    uart_set_pin(AICAM_UART_PORT, _tx, _rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    _ready = true;
    ESP_LOGI(TAG, "Vision AI UART%d up: TX=%d RX=%d @ %lu", (int)AICAM_UART_PORT,
             _tx, _rx, (unsigned long)_baud);
}

void AiCamUartTransport::txWrite(const char *data, int len) {
    if (!_ready) return;
    uart_write_bytes(AICAM_UART_PORT, data, len);
}

int AiCamUartTransport::rxAvailable() {
    if (!_ready) return 0;
    size_t n = 0;
    uart_get_buffered_data_len(AICAM_UART_PORT, &n);
    return (int)n;
}

int AiCamUartTransport::rxRead(char *data, int len) {
    if (!_ready) return 0;
    int n = uart_read_bytes(AICAM_UART_PORT, (uint8_t *)data, len, 0);
    return n < 0 ? 0 : n;
}
