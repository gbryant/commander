#  cmdr-generated: assembles the STM32 Bluepill firmware for a downstream commander project.
#  Compiles the bluepill runner + commander core + STM32 HAL/clock/USB, plus the FreeRTOS
#  kernel and (for USB-console builds) TinyUSB — pulling each from where it actually lives,
#  so platformio.ini just needs `lib_deps = commander` + build flags.
#
#  Source roots:
#    commander : $COMMANDER_PATH (local dev), else the lib_deps download under .pio/libdeps
#    FreeRTOS  : $FREERTOS_KERNEL_PATH
#    TinyUSB   : $TINYUSB_PATH, else $PICO_SDK_PATH/lib/tinyusb (USB builds only)
import os

Import("env")  # noqa: F821

USB = "COMMANDER_STM32_USB_CONSOLE" in env.subst("$BUILD_FLAGS")


def _commander_root():
    p = os.environ.get("COMMANDER_PATH")
    if p and os.path.isdir(p):
        return p
    cand = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env["PIOENV"], "commander")
    if os.path.isdir(cand):
        return cand
    raise SystemExit("stm32_build.py: commander source not found. Add it to lib_deps, or "
                     "set COMMANDER_PATH to a local checkout.")


def _tinyusb_root():
    cands = [os.environ.get("TINYUSB_PATH")]
    if os.environ.get("PICO_SDK_PATH"):
        cands.append(os.path.join(os.environ["PICO_SDK_PATH"], "lib", "tinyusb"))
    cands.append(os.path.expanduser("~/u-developer/pico-sdk/lib/tinyusb"))
    fsdev = os.path.join("src", "portable", "st", "stm32_fsdev", "dcd_stm32_fsdev.c")
    for c in cands:
        if c and os.path.isfile(os.path.join(c, fsdev)):
            return c
    raise SystemExit("stm32_build.py: TinyUSB not found. Set TINYUSB_PATH.")


# ── commander + bluepill runner ───────────────────────────────────────────────
cmd = _commander_root()
runner_dir = os.path.join(cmd, "runners", "stm32-bluepill")    # FreeRTOSConfig.h, runner.cpp
plat_dir = os.path.join(cmd, "platform", "stm32-bluepill")     # clock.c, usb*.c, tusb_config.h
env.Append(CPPPATH=[cmd, os.path.join(cmd, "include"), os.path.join(cmd, "core"),
                    runner_dir, plat_dir])

srcs = [
    "runners/stm32-bluepill/runner.cpp",
    "core/CommandRegistry.cpp",
    "hal/stm32/hal.cpp",
    "transport/uart/UartTransport.cpp",
    "platform/stm32-bluepill/clock.c",
]
if USB:
    srcs += ["platform/stm32-bluepill/usb.c", "platform/stm32-bluepill/usb_descriptors.c"]
env.BuildSources(os.path.join("$BUILD_DIR", "commander"), cmd,
                 src_filter=["-<*>"] + [f"+<{s}>" for s in srcs])

# ── FreeRTOS kernel ───────────────────────────────────────────────────────────
kernel = os.environ.get("FREERTOS_KERNEL_PATH")
if not kernel or not os.path.isdir(kernel):
    raise SystemExit("stm32_build.py: set FREERTOS_KERNEL_PATH to a FreeRTOS-Kernel checkout.")
env.Append(CPPPATH=[os.path.join(kernel, "include"),
                    os.path.join(kernel, "portable", "GCC", "ARM_CM3")])
env.BuildSources(os.path.join("$BUILD_DIR", "FreeRTOS-Kernel"), kernel, src_filter=[
    "-<*>", "+<tasks.c>", "+<queue.c>", "+<list.c>", "+<timers.c>", "+<event_groups.c>",
    "+<portable/GCC/ARM_CM3/port.c>", "+<portable/MemMang/heap_4.c>",
])

# ── TinyUSB (USB-console builds only) ─────────────────────────────────────────
if USB:
    tu_src = os.path.join(_tinyusb_root(), "src")   # filters below are relative to src/
    env.Append(CPPPATH=[tu_src, plat_dir])          # plat_dir has tusb_config.h
    env.BuildSources(os.path.join("$BUILD_DIR", "TinyUSB"), tu_src, src_filter=[
        "-<*>", "+<tusb.c>", "+<common/tusb_fifo.c>", "+<device/usbd.c>",
        "+<device/usbd_control.c>", "+<class/cdc/cdc_device.c>",
        "+<portable/st/stm32_fsdev/dcd_stm32_fsdev.c>",
    ])
