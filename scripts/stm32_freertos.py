#  Pulls the FreeRTOS kernel (Cortex-M3 port + heap_4) into the bluepill build from
#  $FREERTOS_KERNEL_PATH — the same checkout the Pico/ESP32 builds rely on, rather than
#  copying the kernel into the repo. Wired in via `extra_scripts = pre:scripts/stm32_freertos.py`.
import os

Import("env")  # noqa: F821  (injected by PlatformIO)

kernel = os.environ.get("FREERTOS_KERNEL_PATH")
if not kernel or not os.path.isdir(kernel):
    raise SystemExit(
        "stm32_freertos.py: set FREERTOS_KERNEL_PATH to a FreeRTOS-Kernel checkout "
        "(the same one the Pico build uses). Got: %r" % (kernel,)
    )

port_dir = os.path.join(kernel, "portable", "GCC", "ARM_CM3")
config_dir = os.path.join(env["PROJECT_DIR"], "platform", "stm32-bluepill")

# Headers: kernel API, the ARM_CM3 portmacro, and our FreeRTOSConfig.h.
env.Append(CPPPATH=[os.path.join(kernel, "include"), port_dir, config_dir])

# Compile the kernel sources into the firmware (they live outside the project tree,
# so build_src_filter can't reach them — BuildSources handles external roots).
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
