#pragma once
#include <stdint.h>
#include "modules/aicam/Sscma.h"

// ESP32 UART backend for the SSCMA transport seam (Grove Vision AI V2). The Vision
// AI's UART links to the XIAO ESP32-S3 socket on TX=GPIO43 / RX=GPIO44 at 921600,
// which is a *second* UART (the commander console is native USB Serial/JTAG). Uses
// the esp_driver_uart peripheral on UART_NUM_1 — esp-idf types are kept out of this
// header so the app component can include it freely; the driver lives in the .cpp,
// which the runner compiles only when COMMANDER_ENABLE_AICAM is set.
//
// Pins/baud/port are -DAICAM_UART_* overridable (see the .cpp).
class AiCamUartTransport : public ISscmaTransport {
public:
    AiCamUartTransport(int tx = 43, int rx = 44, uint32_t baud = 921600)
        : _tx(tx), _rx(rx), _baud(baud) {}

    void begin() override;                              // installs the UART driver
    void txWrite(const char *data, int len) override;
    int  rxAvailable() override;
    int  rxRead(char *data, int len) override;

private:
    int      _tx;
    int      _rx;
    uint32_t _baud;
    bool     _ready = false;
};
