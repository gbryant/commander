# Run via `cmake -DOUT=<header> -DCOUNTER=<file> [-DNAME=<name>] -P VersionStamp.cmake`.
# Increments the build counter and writes a header with BUILD_NUMBER + a timestamp
# (and BUILD_NAME when NAME is given), so `version` reports which firmware is
# running (confirms OTAs). Rewritten each build → only the version TU recompiles.

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
set(_body "#pragma once\n")
if(NAME)
    string(APPEND _body "#define BUILD_NAME \"${NAME}\"\n")
endif()
string(APPEND _body "#define BUILD_NUMBER ${n}\n#define BUILD_COMMIT \"${ts}\"\n")
file(WRITE "${OUT}" "${_body}")
message(STATUS "[version] ${NAME} build #${n} (${ts})")
