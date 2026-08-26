#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// BTT TFT35-E3 V3.0: STM32F207VC, Cortex-M3, 128 KB SRAM, 256 KB flash.
// Kernel + GCC/ARM_CM3 port are pulled from $FREERTOS_KERNEL_PATH by
// scripts/stm32_tft35_freertos.py — same Cortex-M3 port the Bluepill uses.
//
// configCPU_CLOCK_HZ is 16 MHz (HSI, no PLL yet) — see platform/btt-tft35/clock.c. Raise
// this once HSE+PLL bring-up lands.

#define configUSE_PREEMPTION                1
#define configUSE_TIME_SLICING              1
#define configCPU_CLOCK_HZ                  ( 16000000UL )   // set by stm32_clock_init()
#define configTICK_RATE_HZ                  ( 1000 )
#define configMAX_PRIORITIES                5
#define configMINIMAL_STACK_SIZE            128              // words (×4 = 512 B)
#define configMAX_TASK_NAME_LEN             16
#define configTICK_TYPE_WIDTH_IN_BITS       TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD             1

// 128 KB SRAM total — plenty of headroom vs. the Bluepill's 20 KB; still conservative
// here since nothing but the shell runs yet (LCD/touch will want their own budget).
#define configTOTAL_HEAP_SIZE               ( 32 * 1024 )
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configSUPPORT_STATIC_ALLOCATION     0

#define configUSE_MUTEXES                   1
#define configUSE_RECURSIVE_MUTEXES         1
#define configUSE_COUNTING_SEMAPHORES       1
#define configUSE_TASK_NOTIFICATIONS        1
#define configQUEUE_REGISTRY_SIZE           0

// Software timers off (we don't use them; saves RAM) — matches the other native ports.
#define configUSE_TIMERS                    0

// Diagnostics
#define configCHECK_FOR_STACK_OVERFLOW      2
#define configUSE_MALLOC_FAILED_HOOK        1
#define configUSE_IDLE_HOOK                 0
#define configUSE_TICK_HOOK                 0
#define configUSE_TRACE_FACILITY            0

#define configASSERT( x ) \
    if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ) {} }

// --- Cortex-M3 (STM32F207) interrupt priorities -------------------------------
// F207 NVIC implements 4 priority bits, same as F103.
#define configPRIO_BITS                             4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

// Map the FreeRTOS port's handlers onto the CMSIS startup vector-table names.
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTimerPendFunctionCall      0

#endif /* FREERTOS_CONFIG_H */
