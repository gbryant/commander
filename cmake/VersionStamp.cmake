# Run via `cmake -DOUT=<header> -DCOUNTER=<file> -P VersionStamp.cmake`.
# Increments the build counter and writes a header with BUILD_NUMBER + a
# timestamp, so `version` reports which firmware is running (confirms OTAs).
# The header is only rewritten when the values change, so the version TU
# recompiles each build but nothing else does.

set(n 0)
if(EXISTS "${COUNTER}")
    file(READ "${COUNTER}" n)
    string(STRIP "${n}" n)
endif()
if(NOT n MATCHES "^[0-9]+$")
    set(n 0)
endif()
math(EXPR n "${n} + 1")
file(WRITE "${COUNTER}" "${n}\n")

string(TIMESTAMP ts "%Y-%m-%d %H:%M")
file(WRITE "${OUT}"
    "#pragma once\n#define BUILD_NUMBER ${n}\n#define BUILD_COMMIT \"${ts}\"\n")
message(STATUS "[version] build #${n} (${ts})")
