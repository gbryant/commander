#pragma once

// BUILD_NAME (firmware/project name), BUILD_NUMBER (integer), and BUILD_COMMIT
// (string — a timestamp or git ref) are injected by the build per burn so the
// `version` command can report which firmware is running (e.g. to confirm an
// OTA). On PlatformIO that's scripts/version_stamp.py (a pre-build extra_script,
// via -D); on CMake (pico/esp32) a pre-build step regenerates commander_build.h,
// picked up here via __has_include. The fallbacks below keep the version command
// working where nothing injects them.
#if defined(__has_include)
#  if __has_include("commander_build.h")
#    include "commander_build.h"
#  endif
#endif
#ifndef BUILD_NAME
#define BUILD_NAME "commander"
#endif
#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif
#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown"
#endif
