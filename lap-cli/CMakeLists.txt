# lap - fine-grained edit recorder for AI agents (C11, zero dependencies)
#
#   cmake -B build && cmake --build build
#   ctest --test-dir build          # unit tests (+ e2e on POSIX)
#   cmake --install build           # installs bin/lap (see CMAKE_INSTALL_PREFIX)
#   cmake --build build --target uninstall
#
# This is the cross-platform build (use it for MSVC/MinGW on Windows);
# the plain Makefile stays the quick path on POSIX.

cmake_minimum_required(VERSION 3.16)
project(lap VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LAP_LIB_SOURCES
    src/arena.c
    src/str.c
    src/sha256.c
    src/json.c
    src/platform.c
    src/ignore.c
    src/diff.c
    src/rec.c
    src/repo.c
    src/cmd_common.c
    src/cmd_init.c
    src/cmd_status.c
    src/cmd_commit.c
    src/cmd_log.c
    src/cmd_show.c
    src/cmd_search.c
    src/cmd_session.c
    src/cmd_verify.c
    src/cmd_rebuild.c
    src/idx.c
    src/snap.c
)

add_executable(lap ${LAP_LIB_SOURCES} src/main.c)

if(MSVC)
    target_compile_options(lap PRIVATE /W4)
else()
    target_compile_options(lap PRIVATE -Wall -Wextra)
endif()

# ---- tests -----------------------------------------------------------------
enable_testing()

file(GLOB LAP_UNIT_SOURCES tests/unit/*.c)
add_executable(lap-unit-tests ${LAP_LIB_SOURCES} ${LAP_UNIT_SOURCES})
target_include_directories(lap-unit-tests PRIVATE src)
if(MSVC)
    target_compile_options(lap-unit-tests PRIVATE /W4)
else()
    target_compile_options(lap-unit-tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME unit COMMAND lap-unit-tests)
if(UNIX)
    add_test(NAME e2e
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run.sh)
    set_tests_properties(e2e PROPERTIES
        ENVIRONMENT "LAP=$<TARGET_FILE:lap>")
endif()

# ---- install / uninstall ---------------------------------------------------
include(GNUInstallDirs)
install(TARGETS lap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# CMake has no built-in uninstall; remove everything recorded in this build's
# install_manifest.txt.
if(NOT TARGET uninstall)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND}
                -DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake)
endif()
