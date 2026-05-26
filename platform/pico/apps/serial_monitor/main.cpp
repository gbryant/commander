#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

static inline void dbg(const char *s) { uart_puts(uart1, s); }

static void mainTask(void *) {
    dbg("[task] entered\r\n");
    for (;;) {
        dbg("[task] alive\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" {
    void vApplicationMallocFailedHook(void) {
        dbg("[freertos] malloc failed\r\n");
        for (;;) tight_loop_contents();
    }
    void vApplicationStackOverflowHook(TaskHandle_t, char *name) {
        dbg("[freertos] stack overflow: ");
        dbg(name ? name : "?");
        dbg("\r\n");
        for (;;) tight_loop_contents();
    }
    void vApplicationIdleHook(void) {
        static bool once = false;
        if (!once) { once = true; dbg("[freertos] idle running\r\n"); }
    }
}

#ifdef PICO_FAULT_DIAG
#include "hardware/exception.h"

static void dbg_hex32(uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[11];
    buf[0]='0'; buf[1]='x';
    for (int i = 9; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    buf[10] = '\0';
    uart_puts(uart1, buf);
}

// HFSR 0x80000000=DEBUGEVT(panic/BKPT) 0x40000000=FORCED(escalated) 0x2=VECTTBL
// CFSR 0x00080000=NOCP 0x00040000=INVPC 0x00020000=INVSTATE 0x00010000=UNDEFINSTR
static void fault_hardfault(void) {
    uint32_t hfsr = *(volatile uint32_t*)0xE000ED2C;
    uint32_t cfsr = *(volatile uint32_t*)0xE000ED28;
    dbg("[fault] hardfault HFSR="); dbg_hex32(hfsr);
    dbg(" CFSR="); dbg_hex32(cfsr); dbg("\r\n");
    for (;;) tight_loop_contents();
}
static void fault_usagefault(void) { dbg("[fault] usagefault\r\n"); for (;;) tight_loop_contents(); }
static void fault_busfault(void)   { dbg("[fault] busfault\r\n");   for (;;) tight_loop_contents(); }
static void fault_memmanage(void)  { dbg("[fault] memmanage\r\n");  for (;;) tight_loop_contents(); }
#endif /* PICO_FAULT_DIAG */

int main() {
    uart_init(uart1, 115200);
    gpio_set_function(4, GPIO_FUNC_UART);
    gpio_set_function(5, GPIO_FUNC_UART);
    uart_set_format(uart1, 8, 1, UART_PARITY_NONE);

#ifdef PICO_FAULT_DIAG
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION,   fault_hardfault);
    exception_set_exclusive_handler(USAGEFAULT_EXCEPTION,  fault_usagefault);
    exception_set_exclusive_handler(BUSFAULT_EXCEPTION,    fault_busfault);
    exception_set_exclusive_handler(MEMMANAGE_EXCEPTION,   fault_memmanage);
#endif

    BaseType_t rc = xTaskCreate(mainTask, "main", 4096, nullptr, 1, nullptr);
    dbg(rc == pdPASS ? "xTaskCreate ok\r\n" : "xTaskCreate FAILED\r\n");

    vTaskStartScheduler();

    for (;;) {}
}
