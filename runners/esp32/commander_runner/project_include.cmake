# Auto-included by ESP-IDF at the top-level project scope. Provides
# commander_stamp_version(), which the consumer's root CMakeLists.txt calls
# *after* project() to stamp a build number into the firmware.
#
# This is the esp32 counterpart of commander_stamp_version(TARGET) in
# cmake/GenerateScripts.cmake (pico/PlatformIO-CMake): same name and behavior, but
# project-level and argument-less. ESP-IDF processes each component in a sub-scope
# and won't promote a component's custom ALL target into the default build, so the
# stamp can't live in the runner component's CMakeLists.txt — it must be added at
# the project level with an explicit dependency on the firmware.
#
#   commander_stamp_version()  →  regenerates commander_build.h (BUILD_NAME =
#   project name, BUILD_NUMBER, timestamp) before every build, makes it visible to
#   version.h, and writes ./.build_number so `bum-ota` can confirm an OTA took.

function(commander_stamp_version)
    if(NOT DEFINED COMMANDER_ROOT)
        message(FATAL_ERROR "commander_stamp_version(): COMMANDER_ROOT is not set")
    endif()
    set(_hdr     "${CMAKE_BINARY_DIR}/commander_build.h")
    set(_counter "${CMAKE_SOURCE_DIR}/.build_number")
    set(_script  "${COMMANDER_ROOT}/cmake/VersionStamp.cmake")
    set(_name    "${CMAKE_PROJECT_NAME}")             # BUILD_NAME — the project's identity

    # Seed the header at configure time so it exists for the very first compile.
    execute_process(COMMAND ${CMAKE_COMMAND}
        -DOUT=${_hdr} -DCOUNTER=${_counter} -DNAME=${_name} -P ${_script})

    # Re-stamp before every build. `_always_versionstamp` is a SYMBOLIC output that
    # never exists on disk, so this command is perpetually out of date — forcing the
    # header (and thus the version translation unit) to rebuild and re-increment.
    set_source_files_properties("${CMAKE_BINARY_DIR}/_always_versionstamp"
        PROPERTIES SYMBOLIC TRUE)
    add_custom_command(
        OUTPUT  "${_hdr}" "${CMAKE_BINARY_DIR}/_always_versionstamp"
        COMMAND ${CMAKE_COMMAND} -DOUT=${_hdr} -DCOUNTER=${_counter} -DNAME=${_name} -P ${_script}
        COMMENT "[version] stamping build number"
        VERBATIM)
    add_custom_target(commander_stamp_version_tgt DEPENDS "${_hdr}")

    # version.h does __has_include("commander_build.h"); the build dir must be on
    # the include path of the runner (PUBLIC → propagates to the main component,
    # which compiles SystemModule's `version` command). Order the stamp first.
    idf_component_get_property(_runner_lib commander_runner COMPONENT_LIB)
    target_include_directories(${_runner_lib} PUBLIC "${CMAKE_BINARY_DIR}")
    add_dependencies(${_runner_lib} commander_stamp_version_tgt)
endfunction()
