# commander_generate_scripts(TARGET)
#
# Generates bum, build, upload, monitor, and bum-ota scripts in
# CMAKE_SOURCE_DIR (the consumer's project root).
#
# Call after add_executable and target_link_libraries:
#
#   add_executable(my_robot main.cpp)
#   target_link_libraries(my_robot PRIVATE commander::pico_runner)
#   commander_generate_scripts(my_robot)
#
# Scripts reference find_port.py and ota_push.py from the commander source tree
# at the path recorded during cmake configure — stable for FetchContent builds.

# Capture paths at include time; CMAKE_CURRENT_LIST_DIR changes inside functions.
set(_CMDR_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/scripts" CACHE INTERNAL "")
get_filename_component(_CMDR_REPO_SCRIPTS_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../scripts" ABSOLUTE CACHE)

function(commander_generate_scripts TARGET)
    if(NOT DEFINED PICO_BOARD)
        message(WARNING "[commander] PICO_BOARD not set — skipping script generation")
        return()
    endif()

    # Board short-name + port hint used in script names and find_port.py
    if(PICO_BOARD MATCHES "pico2")
        set(_short "pico2")
        set(_port  "pico2")
    else()
        set(_short "pico")
        set(_port  "pico")
    endif()
    set(_host "${TARGET}.local")

    # Template substitution variables (@CMDR_*@ syntax, @ONLY mode)
    set(CMDR_APP         ${TARGET})
    set(CMDR_BOARD       ${PICO_BOARD})
    set(CMDR_BOARD_SHORT ${_short})
    set(CMDR_BUILD_DIR   ${CMAKE_BINARY_DIR})
    set(CMDR_SOURCE_DIR  ${CMAKE_SOURCE_DIR})
    set(CMDR_PORT_HINT   ${_port})
    set(CMDR_OTA_HOST    ${_host})
    set(CMDR_SCRIPTS     ${_CMDR_REPO_SCRIPTS_DIR})

    # The controller module (Bluepad32) reads $BLUEPAD32_PATH at configure time,
    # and unlike PICO_SDK_PATH it is not remembered in the CMake cache — so bake
    # the configured value into `build` to keep the standard scripts self-contained
    # (no shell-env setup needed). Empty for non-controller projects.
    set(CMDR_ENV_EXPORTS "")
    if(COMMANDER_ENABLE_CONTROLLER AND DEFINED ENV{BLUEPAD32_PATH})
        set(CMDR_ENV_EXPORTS "export BLUEPAD32_PATH=\"$ENV{BLUEPAD32_PATH}\"")
    endif()

    foreach(_name build upload monitor bum bum-ota)
        set(_out "${CMAKE_SOURCE_DIR}/${_name}")
        configure_file(
            "${_CMDR_TEMPLATE_DIR}/${_name}.sh.in"
            "${_out}"
            @ONLY
        )
        file(CHMOD "${_out}"
            PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
        )
    endforeach()

    message(STATUS "[commander] Scripts written to ${CMAKE_SOURCE_DIR}:")
    message(STATUS "  bum  build  upload  monitor  bum-ota")
endfunction()
