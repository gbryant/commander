#  Pulls the FreeRTOS kernel (Cortex-M3 port + heap_4) into the btt-tft35 build from
#  $FREERTOS_KERNEL_PATH — same checkout the Bluepill/Pico/ESP32 builds rely on. F207 is
#  Cortex-M3 like F103, so this reuses the same GCC/ARM_CM3 port (see
#  scripts/stm32_freertos.py, which this mirrors for the Bluepill env).
import os

Import("env")  # noqa: F821  (injected by PlatformIO)

kernel = os.environ.get("FREERTOS_KERNEL_PATH")
if not kernel or not os.path.isdir(kernel):
    raise SystemExit(
        "stm32_tft35_freertos.py: set FREERTOS_KERNEL_PATH to a FreeRTOS-Kernel checkout "
        "(the same one the Pico/Bluepill builds use). Got: %r" % (kernel,)
    )

port_dir = os.path.join(kernel, "portable", "GCC", "ARM_CM3")
config_dir = os.path.join(env["PROJECT_DIR"], "runners", "btt-tft35")

env.Append(CPPPATH=[os.path.join(kernel, "include"), port_dir, config_dir])

env.BuildSources(
    os.path.join("$BUILD_DIR", "FreeRTOS-Kernel"),
    kernel,
    src_filter=[
        "-<*>",
        "+<tasks.c>",
        "+<queue.c>",
        "+<list.c>",
        "+<timers.c>",
        "+<event_groups.c>",
        "+<portable/GCC/ARM_CM3/port.c>",
        "+<portable/MemMang/heap_4.c>",
    ],
)
