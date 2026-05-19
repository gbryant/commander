#pragma once
#include <assert.h>
#define configASSERT(x) assert(x)

// SMP — both RP2350 cores
#define configNUMBER_OF_CORES                       2
#define configUSE_CORE_AFFINITY                     1
#define configRUN_MULTIPLE_PRIORITIES               1

// Pico-SDK interop: lets pico-sdk sync primitives and time functions
// yield correctly under FreeRTOS instead of busy-waiting
#define configSUPPORT_PICO_SYNC_INTEROP             1
#define configSUPPORT_PICO_TIME_INTEROP             1

#define configUSE_PREEMPTION                        1
#define configUSE_TICKLESS_IDLE                     0
#define configCPU_CLOCK_HZ                          150000000
#define configTICK_RATE_HZ                          1000
#define configMAX_PRIORITIES                        8
#define configMINIMAL_STACK_SIZE                    256
// RP2350 has 520KB RAM, RP2040 has 264KB
#ifdef PICO_RP2350
#define configTOTAL_HEAP_SIZE                       (200 * 1024)
#else
#define configTOTAL_HEAP_SIZE                       (128 * 1024)
#endif
#define configMAX_TASK_NAME_LEN                     16
#define configUSE_16_BIT_TICKS                      0
#define configIDLE_SHOULD_YIELD                     1
#define configUSE_MUTEXES                           1
#define configUSE_RECURSIVE_MUTEXES                 1
#define configUSE_COUNTING_SEMAPHORES               1
#define configUSE_TASK_NOTIFICATIONS                1
#define configQUEUE_REGISTRY_SIZE                   8
#define configUSE_TRACE_FACILITY                    0
#define configUSE_STATS_FORMATTING_FUNCTIONS        0
#define configUSE_NEWLIB_REENTRANT                  0
#define configENABLE_BACKWARD_COMPATIBILITY         0
// lwIP FreeRTOS port (pico-sdk/lib/lwip) references the pre-v8.0 name
#define portTICK_RATE_MS                            portTICK_PERIOD_MS
#define configUSE_IDLE_HOOK                         0
#define configUSE_TICK_HOOK                         0
#define configUSE_PASSIVE_IDLE_HOOK                 0
#define configUSE_MALLOC_FAILED_HOOK                1
#define configCHECK_FOR_STACK_OVERFLOW              2

// Software timers
#define configUSE_TIMERS                            1
#define configTIMER_TASK_PRIORITY                   (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                    10
#define configTIMER_TASK_STACK_DEPTH                1024

// RP2350 (Cortex-M33) only — not present on RP2040 (Cortex-M0+)
#ifdef PICO_RP2350
#define configENABLE_FPU                            1
#define configENABLE_MPU                            0
#define configENABLE_TRUSTZONE                      0  // NTZ port
#endif

// Interrupt priority bits: M33 (RP2350) has 3, M0+ (RP2040) has 2
#ifdef PICO_RP2350
#define configPRIO_BITS                             3
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     7
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 2
#else
#define configPRIO_BITS                             2
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     3
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 1
#endif
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))


#define INCLUDE_vTaskPrioritySet                    1
#define INCLUDE_uxTaskPriorityGet                   1
#define INCLUDE_vTaskDelete                         1
#define INCLUDE_vTaskSuspend                        1
#define INCLUDE_xResumeFromISR                      1
#define INCLUDE_vTaskDelayUntil                     1
#define INCLUDE_vTaskDelay                          1
#define INCLUDE_xTaskGetSchedulerState              1
#define INCLUDE_xTaskGetCurrentTaskHandle           1
#define INCLUDE_uxTaskGetStackHighWaterMark         1
#define INCLUDE_xTaskGetIdleTaskHandle              1
#define INCLUDE_eTaskGetState                       1
#define INCLUDE_xEventGroupSetBitFromISR            1
#define INCLUDE_xTimerPendFunctionCall              1
#define INCLUDE_xTaskAbortDelay                     1
#define INCLUDE_xTaskGetHandle                      1
#define INCLUDE_xTaskResumeFromISR                  1
#define INCLUDE_xSemaphoreGetMutexHolder            1
