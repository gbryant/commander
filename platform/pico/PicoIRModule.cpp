// PicoIRModule::init() — kept out of the header so the generated ir_rx.pio.h
// dependency stays encapsulated in the commander_pico_ir CMake target (which
// runs pico_generate_pio_header). Consumers include only the clean header.
#include "PicoIRModule.h"
#include "ir_rx.pio.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

void PicoIRModule::init() {
    // Configure PIO but leave the SM disabled until launch() — active PIO during
    // the WiFi WPA2 handshake can cause BADAUTH (-7).
    _pio        = pio1;  // pio0 is reserved for the CYW43 SPI bus
    _sm         = pio_claim_unused_sm(_pio, true);
    uint offset = pio_add_program(_pio, &ir_rx_program);
    ir_rx_program_init(_pio, _sm, offset, _gpio);
    pio_sm_set_enabled(_pio, _sm, false);
    gpio_pull_up(_gpio);
}
