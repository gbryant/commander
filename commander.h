#pragma once
#include "core/CommandRegistry.h"
#include "transport/uart/UartTransport.h"

// ── App configuration ─────────────────────────────────────────────────────
// Returned by commander_config(). Unset fields keep their defaults.
struct CommanderConfig {
    // WiFi — leave wifi_ssid null to skip all networking
    const char *wifi_ssid     = nullptr;
    const char *wifi_password = nullptr;
    const char *hostname      = "commander";  // mDNS / DHCP hostname

    // I2C — set i2c_sda to -1 to skip hal_i2c_init()
    int      i2c_sda = -1;
    int      i2c_scl = -1;
    uint32_t i2c_hz  = 100000;

    // UART transport
    uint32_t    uart_baud     = 115200;
    const char *uart_greeting = nullptr;

    // Telnet transport — started when WiFi connects
    bool        enable_telnet   = true;
    const char *telnet_greeting = nullptr;  // falls back to hostname if null

    bool debug = false;
};

// ── Required app callbacks ────────────────────────────────────────────────

// Return hardware and transport configuration for this app.
extern "C" CommanderConfig commander_config();

// Register modules with the command registry.
// Called inside the main FreeRTOS task, after HAL init.
extern "C" void commander_setup(CommandRegistry &reg);

// ── Optional hooks (runner provides weak no-op defaults) ──────────────────

// Called by main() before stdio_init_all(), before the FreeRTOS scheduler.
// Use for pre-scheduler hardware checks (e.g. BOOTSEL / watchdog flags).
extern "C" void commander_early_init();

// Called after commander_setup(), before the UART task is created.
// Use to add module tickers: uart.addTicker(myModule).
extern "C" void commander_on_uart_ready(UartTransport &uart);

// Called after WiFi connects successfully, still inside the main FreeRTOS task.
// Use for operations that must follow WiFi (e.g. launching PIO on core1).
extern "C" void commander_on_wifi_connected();
