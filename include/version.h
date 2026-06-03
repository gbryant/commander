#pragma once

// Human-readable firmware name — edit to identify this build.
#define BUILD_NAME "commander"

// BUILD_NUMBER (integer) and BUILD_COMMIT (string, a timestamp or git ref) are
// injected by the build per burn so `version` can confirm which firmware is
// running (e.g. after an OTA). On PlatformIO that's scripts/version_stamp.py
// (a pre-build extra_script). The fallbacks below keep the version command
// working where nothing injects them.
#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif
#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown"
#endif
