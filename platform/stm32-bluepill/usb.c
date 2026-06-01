// USB hardware bring-up for the F103 fsdev peripheral + the TinyUSB device task.
// The 48 MHz USB clock is already set by stm32_clock_init() (USBPRE in clock.c).
#include "stm32f1xx.h"
#include "tusb.h"

// Enable the USB peripheral and IRQ, after nudging the host to re-enumerate.
void usb_hw_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

    // The Bluepill has no software-controlled D+ pull-up, so force a disconnect:
    // drive PA12 (D+) low briefly, then release it to the USB macrocell, nudging the host
    // to re-enumerate. A proper 1.5k pull-up on D+ makes this reliable on every (re)connect;
    // with a weak/wrong R10 the cleanest disconnect comes from a hardware reset (NRST).
    GPIOA->CRH = (GPIOA->CRH & ~(0xFu << 16)) | (0x2u << 16);   // PA12 output PP, 2 MHz
    GPIOA->BSRR = (1u << (12 + 16));                            // PA12 = 0
    for (volatile uint32_t i = 0; i < 400000; i++) { }          // ~a few ms @ 72 MHz
    GPIOA->CRH = (GPIOA->CRH & ~(0xFFu << 12)) | (0x44u << 12); // PA11/PA12 input floating

    RCC->APB1ENR |= RCC_APB1ENR_USBEN;                          // USB peripheral clock

    // FreeRTOS-safe priority (numeric >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY).
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 6);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
}

// F103 USB low-priority interrupt -> TinyUSB.
void USB_LP_CAN1_RX0_IRQHandler(void) {
    tud_int_handler(0);
}

// Dedicated device task: init the stack, then pump it forever.
void usbd_task(void *arg) {
    (void)arg;
    tusb_init();
    for (;;) {
        tud_task();
    }
}
