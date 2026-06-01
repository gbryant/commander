#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// STM32F103C8 "Bluepill": Cortex-M3 @ 72 MHz, 20 KB SRAM, 64 KB flash.
// Kernel + GCC/ARM_CM3 port are pulled from $FREERTOS_KERNEL_PATH by
// scripts/stm32_freertos.py.

#define configUSE_PREEMPTION                1
#define configUSE_TIME_SLICING              1
#define configCPU_CLOCK_HZ                  ( 72000000UL )   // set by stm32_clock_init()
#define configTICK_RATE_HZ                  ( 1000 )
#define configMAX_PRIORITIES                5
#define configMINIMAL_STACK_SIZE            128              // words (×4 = 512 B)
#define configMAX_TASK_NAME_LEN             16
#define configTICK_TYPE_WIDTH_IN_BITS       TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD             1

// 20 KB SRAM total; leave room for task stacks, .bss/.data, and (later) TinyUSB RAM.
#define configTOTAL_HEAP_SIZE               ( 10 * 1024 )
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configSUPPORT_STATIC_ALLOCATION     0

#define configUSE_MUTEXES                   1
#define configUSE_RECURSIVE_MUTEXES         1
#define configUSE_COUNTING_SEMAPHORES       1
#define configUSE_TASK_NOTIFICATIONS        1
#define configQUEUE_REGISTRY_SIZE           0

// Software timers off (we don't use them; saves RAM) — matches the Uno.
#define configUSE_TIMERS                    0

// Diagnostics
#define configCHECK_FOR_STACK_OVERFLOW      2
#define configUSE_MALLOC_FAILED_HOOK        1
#define configUSE_IDLE_HOOK                 0
#define configUSE_TICK_HOOK                 0
#define configUSE_TRACE_FACILITY            0

#define configASSERT( x ) \
    if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ) {} }

// --- Cortex-M3 (STM32F103) interrupt priorities -------------------------------
// F103 NVIC implements 4 priority bits.
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
