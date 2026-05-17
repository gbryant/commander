#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <avr/io.h>

#define configUSE_PORT_DELAY                1
#define configUSE_PREEMPTION                1
#define configCPU_CLOCK_HZ                  ( ( uint32_t ) F_CPU )
#define configTICK_TYPE_WIDTH_IN_BITS       TICK_TYPE_WIDTH_16_BITS
#define configMAX_PRIORITIES                4
#define configMAX_TASK_NAME_LEN             16
#define configSTACK_DEPTH_TYPE              uint16_t

// Uno has 2KB SRAM — keep stacks minimal, use static allocation.
#define configMINIMAL_STACK_SIZE            128
#define configCHECK_FOR_STACK_OVERFLOW      1
#define configUSE_TRACE_FACILITY            0
#define configUSE_MUTEXES                   1
#define configUSE_RECURSIVE_MUTEXES         1
#define configUSE_COUNTING_SEMAPHORES       1
#define configUSE_TIME_SLICING              1
#define configUSE_QUEUE_SETS                0
#define configUSE_APPLICATION_TASK_TAG      0
#define configUSE_MALLOC_FAILED_HOOK        1
#define configQUEUE_REGISTRY_SIZE           0
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configSUPPORT_STATIC_ALLOCATION     1
#define configUSE_IDLE_HOOK                 1
#define configIDLE_SHOULD_YIELD             1
#define configUSE_TICK_HOOK                 0

// Timers disabled: timer task + queue consume ~480 bytes of the ~560-byte
// heap budget on the Uno, starving vTaskStartScheduler of the idle task.
#define configUSE_TIMERS                    0
#define configTIMER_TASK_PRIORITY           ( configMAX_PRIORITIES - 1 )
#define configTIMER_TASK_STACK_DEPTH        92
#define configTIMER_QUEUE_LENGTH            10

#define configMAX(a,b)  ({ __typeof__(a) _a=(a); __typeof__(b) _b=(b); _a>_b?_a:_b; })
#define configMIN(a,b)  ({ __typeof__(a) _a=(a); __typeof__(b) _b=(b); _a<_b?_a:_b; })

#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_xTaskResumeFromISR          1
#define INCLUDE_xTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      0
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle      0
#define INCLUDE_eTaskGetState               0
#define INCLUDE_xEventGroupSetBitFromISR    1
#define INCLUDE_xTimerPendFunctionCall      0
#define INCLUDE_xTaskAbortDelay             0
#define INCLUDE_xTaskGetHandle              0

#endif /* FREERTOS_CONFIG_H */
