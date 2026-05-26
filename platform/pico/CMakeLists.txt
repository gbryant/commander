cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico_w CACHE STRING "Board type")
include(${CMAKE_CURRENT_LIST_DIR}/../../pico_sdk_import.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../FreeRTOS_Kernel_import.cmake)

project(commander C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ── Commander library + runner targets ───────────────────────────────────────
# Root CMakeLists.txt defines commander::* library targets and commander::pico_runner.
# runners/pico/ owns FreeRTOSConfig.h and lwipopts.h; no platform/pico/ hacks needed.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. commander_lib)

# --------------------------------------------------------------------------
# App selection — must be set explicitly at configure time.
# Example: cmake -B build -DCOMMANDER_APP=commander -DPICO_BOARD=pico_w
# --------------------------------------------------------------------------
set(COMMANDER_APP "" CACHE STRING "App to build (e.g. commander, serial_monitor)")
if(NOT COMMANDER_APP)
    message(FATAL_ERROR
        "COMMANDER_APP not set. Pass -DCOMMANDER_APP=<name>.\n"
        "Available apps: ${CMAKE_CURRENT_LIST_DIR}/apps/")
endif()
set(APP_DIR ${CMAKE_CURRENT_LIST_DIR}/apps/${COMMANDER_APP})
if(NOT EXISTS ${APP_DIR}/app.cmake)
    message(FATAL_ERROR "No app.cmake found at ${APP_DIR}/app.cmake")
endif()
message(STATUS "Building app '${COMMANDER_APP}' for board '${PICO_BOARD}'")

# --------------------------------------------------------------------------
# pico_fota_bootloader — must come after pico_sdk_init()
# --------------------------------------------------------------------------
if(DEFINED ENV{PFB_PATH})
    set(PFB_PATH "$ENV{PFB_PATH}")
else()
    set(PFB_PATH "$ENV{HOME}/u-developer/pico_fota_bootloader")
endif()
set(PFB_WITH_IMAGE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(PFB_WITH_SHA256_HASHING   ON  CACHE BOOL "" FORCE)
add_subdirectory(${PFB_PATH} pfb_build)

# --------------------------------------------------------------------------
# Git metadata
# --------------------------------------------------------------------------
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

# --------------------------------------------------------------------------
# Load app config — sets APP_SOURCES, APP_LINK_LIBS, APP_PIO, APP_USE_PFB
# PICO_PLATFORM_DIR and PROJECT_ROOT_DIR are stable path anchors for app.cmake
# --------------------------------------------------------------------------
set(PICO_PLATFORM_DIR ${CMAKE_CURRENT_LIST_DIR})
set(PROJECT_ROOT_DIR  ${CMAKE_CURRENT_LIST_DIR}/../..)
set(APP_SOURCES      "")
set(APP_LINK_LIBS    "")
set(APP_PIO          "")
set(APP_USE_PFB      OFF)
set(APP_USE_FREERTOS ON)
set(APP_COMPILE_DEFS "")
include(${APP_DIR}/app.cmake)

# --------------------------------------------------------------------------
# Target
# --------------------------------------------------------------------------
add_executable(commander
    ${APP_DIR}/main.cpp
    ${APP_SOURCES}
)

foreach(pio_src ${APP_PIO})
    pico_generate_pio_header(commander ${pio_src})
endforeach()

target_compile_definitions(commander PRIVATE
    CYW43_TASK_STACK_SIZE=2048
    BUILD_COMMIT="${GIT_COMMIT}"
    BUILD_NUMBER=${GIT_BUILD_NUMBER}
    COMMANDER_APP="${COMMANDER_APP}"
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=20
    PICO_DEFAULT_UART_RX_PIN=21
    ${APP_COMPILE_DEFS}
)

target_include_directories(commander PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}  # FreeRTOSConfig.h, lwipopts.h, secrets.h
    ${APP_DIR}                 # app-local headers
)

if(APP_USE_FREERTOS)
    target_link_libraries(commander
        pico_stdlib
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        ${APP_LINK_LIBS}
    )
else()
    target_link_libraries(commander
        pico_stdlib
        ${APP_LINK_LIBS}
    )
endif()

pico_enable_stdio_usb(commander 1)
pico_enable_stdio_uart(commander 1)
pico_add_extra_outputs(commander)

if(APP_USE_PFB)
    pfb_compile_with_bootloader(commander)
endif()
