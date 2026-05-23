#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define IR_PIN 22

int main() {
    stdio_init_all();
    sleep_ms(2000);  // let USB CDC enumerate

    gpio_init(IR_PIN);
    gpio_set_dir(IR_PIN, GPIO_IN);
    gpio_pull_up(IR_PIN);  // TSOP output is open-drain; pull-up required

    printf("IR pin test — GP%d\n", IR_PIN);
    printf("idle = HIGH (1), receiving IR = LOW (0)\n");
    printf("initial state: %d\n\n", (int)gpio_get(IR_PIN));

    bool     last        = gpio_get(IR_PIN);
    uint32_t transitions = 0;
    uint32_t last_report = to_ms_since_boot(get_absolute_time());

    for (;;) {
        bool now = gpio_get(IR_PIN);

        if (now != last) {
            transitions++;
            printf("%d", (int)now);  // compact: stream of 0/1 on transitions
            last = now;
        }

        // Periodic status line every 3 s so we know it's alive
        uint32_t t = to_ms_since_boot(get_absolute_time());
        if (t - last_report >= 3000) {
            printf("\n[%lu s] GP%d=%d  transitions=%lu\n",
                   t / 1000, IR_PIN, (int)now, transitions);
            last_report = t;
        }

        sleep_us(20);  // ~50 kHz poll — fast enough to see IR pulses
    }
}
