#pragma once

// Human-readable firmware name — edit to identify this build.
#define BUILD_NAME "commander"

// BUILD_NUMBER (integer) and BUILD_COMMIT (string, a timestamp or git ref) are
// injected by the build per burn so `version` can confirm which firmware is
// running (e.g. after an OTA). On PlatformIO that's scripts/version_stamp.py (a
// pre-build extra_script, via -D); on CMake (pico/esp32) a pre-build step
// regenerates commander_build.h, picked up here. The fallbacks below keep the
// version command working where nothing injects them.
#if defined(__has_include)
#  if __has_include("commander_build.h")
#    include "commander_build.h"
#  endif
#endif
#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif
#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown"
#endif
