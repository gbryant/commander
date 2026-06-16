#pragma once
#include "core/IModule.h"
#include "core/Writer.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

// Bridges a hardware UART to telnet clients.
// Wire the target device's TX pin to this board's rx_pin.
// The `monitor` command streams live UART input to the connected telnet client;
// it exits when the client disconnects (Writer::ok() returns false).
class SerialMonitorModule : public IModule {
public:
    SerialMonitorModule(uart_inst_t *uart, uint8_t rx_pin, uint8_t tx_pin,
                        uint32_t baud = 115200)
        : _uart(uart), _rx_pin(rx_pin), _tx_pin(tx_pin), _baud(baud) {}

    const char *name() const override { return "serial_monitor"; }

    void init() override {
        uart_init(_uart, _baud);
        gpio_set_function(_rx_pin, GPIO_FUNC_UART);
        gpio_set_function(_tx_pin, GPIO_FUNC_UART);
        uart_set_format(_uart, 8, 1, UART_PARITY_NONE);
        uart_set_fifo_enabled(_uart, true);
        printf("[monitor] uart%d rx=GP%d tx=GP%d baud=%u\n",
               uart_get_index(_uart), _rx_pin, _tx_pin, (unsigned)_baud);
    }

    void registerCommands(CommandRegistry &reg) override {
        reg.registerCommand(CMD("monitor", "stream UART input to this session",
                                I2C_NONE, cmdMonitor, this));
    }

    // Stream UART RX to out until the client disconnects (out.ok() → false).
    void stream(Writer &out) {
        out.writeln("monitoring — disconnect to exit");
        char line[128];
        size_t pos = 0;

        while (out.ok()) {
            while (uart_is_readable(_uart)) {
                char c = uart_getc(_uart);
                // Pass through raw; accumulate for local printf too
                char s[2] = {c, '\0'};
                out.write(s);
                if (c == '\n' || pos >= sizeof(line) - 1) {
                    line[pos] = '\0';
                    printf("[mon] %s\n", line);
                    pos = 0;
                } else if (c != '\r') {
                    line[pos++] = c;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        printf("[monitor] client disconnected\n");
    }

private:
    uart_inst_t *_uart;
    uint8_t      _rx_pin;
    uint8_t      _tx_pin;
    uint32_t     _baud;

    static void cmdMonitor(const char *, Writer &out, void *ctx) {
        static_cast<SerialMonitorModule *>(ctx)->stream(out);
    }
};
