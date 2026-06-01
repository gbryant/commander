#  Pulls a minimal TinyUSB (device CDC + STM32 fsdev port) into the bluepill-usb build.
#  Source tree is resolved from $TINYUSB_PATH, else $PICO_SDK_PATH/lib/tinyusb, else the
#  known u-developer checkout — validated by the presence of the fsdev DCD before use.
#  Wired in via `extra_scripts = pre:scripts/stm32_tinyusb.py`.
import os

Import("env")  # noqa: F821  (injected by PlatformIO)

_FSDEV = os.path.join("src", "portable", "st", "stm32_fsdev", "dcd_stm32_fsdev.c")


def _find_tinyusb():
    cands = []
    if os.environ.get("TINYUSB_PATH"):
        cands.append(os.environ["TINYUSB_PATH"])
    if os.environ.get("PICO_SDK_PATH"):
        cands.append(os.path.join(os.environ["PICO_SDK_PATH"], "lib", "tinyusb"))
    cands.append(os.path.expanduser("~/u-developer/pico-sdk/lib/tinyusb"))
    for c in cands:
        if c and os.path.isfile(os.path.join(c, _FSDEV)):
            return c
    raise SystemExit(
        "stm32_tinyusb.py: no TinyUSB checkout found (need %s). "
        "Set TINYUSB_PATH to a tinyusb source tree. Tried: %s"
        % (_FSDEV, ", ".join(p for p in cands if p))
    )


tu = _find_tinyusb()
src = os.path.join(tu, "src")
config_dir = os.path.join(env["PROJECT_DIR"], "platform", "stm32-bluepill")

env.Append(CPPPATH=[src, config_dir])

env.BuildSources(
    os.path.join("$BUILD_DIR", "TinyUSB"),
    src,
    src_filter=[
        "-<*>",
        "+<tusb.c>",
        "+<common/tusb_fifo.c>",
        "+<device/usbd.c>",
        "+<device/usbd_control.c>",
        "+<class/cdc/cdc_device.c>",
        "+<portable/st/stm32_fsdev/dcd_stm32_fsdev.c>",
    ],
)
