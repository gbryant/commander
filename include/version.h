#pragma once

// Human-readable firmware name — edit to identify this build.
#define BUILD_NAME "commander"

// BUILD_NUMBER (integer) and BUILD_COMMIT (string) are injected by CMake
// from git at configure time.  Fallbacks keep the version command working
// on platforms without the CMake git integration (e.g. Arduino/PlatformIO).
#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif
#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown"
#endif
