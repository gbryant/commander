# Demo app component: just main.cpp. The runner (component commander_runner) owns
# app_main, the HAL/transport sources, and the weak hooks. The git stamping feeds
# the `version` command (BUILD_* fall back to 0/unknown via include/version.h).
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BUILD_NUMBER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GIT_COMMIT)
    set(GIT_COMMIT "unknown")
endif()
if(NOT GIT_BUILD_NUMBER)
    set(GIT_BUILD_NUMBER 0)
endif()

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ".."
    REQUIRES commander_runner
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BUILD_COMMIT="${GIT_COMMIT}"
    BUILD_NUMBER=${GIT_BUILD_NUMBER}
)
