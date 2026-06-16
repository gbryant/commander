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
set(COMMANDER_ENABLE_OTA ON CACHE BOOL "" FORCE)
# Keep IR Roomba-wall detection in this reference build. Before add_subdirectory
# so it reaches commander_pico_ir + the executable consistently (gated by
# COMMANDER_IR_WALL — must match across TUs to avoid an ODR mismatch).
add_compile_definitions(COMMANDER_IR_WALL)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. commander_lib)

message(STATUS "Building commander for board '${PICO_BOARD}'")

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
# Target — the commander reference app. Framework boilerplate (main, WiFi,
# FreeRTOS wiring) comes from commander::pico_runner; main.cpp here just
# implements the commander_config()/commander_setup() hooks.
# --------------------------------------------------------------------------
add_executable(commander
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
)

# IR receive uses a PIO program (Roomba virtual-wall + NEC/Sony decode).
pico_generate_pio_header(commander ${CMAKE_CURRENT_LIST_DIR}/ir_rx.pio)

target_compile_definitions(commander PRIVATE
    CYW43_TASK_STACK_SIZE=2048
    BUILD_NAME="${PROJECT_NAME}"
    BUILD_COMMIT="${GIT_COMMIT}"
    BUILD_NUMBER=${GIT_BUILD_NUMBER}
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=20
    PICO_DEFAULT_UART_RX_PIN=21
)

target_include_directories(commander PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}  # secrets.h + app headers (PicoIRModule.h, sdkconfig.h, btstack_config.h)
)

target_link_libraries(commander
    pico_stdlib
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
    commander::pico_runner
    hardware_pio
    hardware_clocks
    pico_multicore
    pico_fota_bootloader_lib
)

pico_enable_stdio_usb(commander 1)
pico_enable_stdio_uart(commander 1)
pico_add_extra_outputs(commander)

# OTA self-flash via pico_fota_bootloader.
pfb_compile_with_bootloader(commander)
