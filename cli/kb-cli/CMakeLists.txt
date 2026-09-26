# kb - local knowledge base over docs, source and papers (C11, zero deps)
#
#   cmake -B build && cmake --build build
#   ctest --test-dir build          # unit tests (+ e2e on POSIX)
#   cmake --install build           # installs bin/kb (see CMAKE_INSTALL_PREFIX)
#   cmake --build build --target uninstall
#
# This is the cross-platform build (use it for MSVC/MinGW on Windows);
# the plain Makefile stays the quick path on POSIX.

cmake_minimum_required(VERSION 3.16)
project(kb VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(KB_LIB_SOURCES
    src/arena.c
    src/str.c
    src/sha256.c
    src/json.c
    src/errdet.c
    src/platform.c
    src/doc.c
    src/store.c
    src/chunk.c
    src/token.c
    src/fts.c
    src/snippet.c
    src/rank.c
    src/cmd_common.c
    src/cmd_init.c
    src/cmd_add.c
    src/cmd_ls.c
    src/cmd_get.c
    src/cmd_search.c
    src/cmd_chunk.c
    src/cmd_rebuild.c
    src/cmd_collections.c
    src/cmd_status.c
    src/cmd_stale.c
    src/cmd_links.c
    src/cmd_maint.c
    src/cmd_forget.c
    src/cmd_sources.c
)

add_executable(kb ${KB_LIB_SOURCES} src/main.c)

if(MSVC)
    target_compile_options(kb PRIVATE /W4)
else()
    target_compile_options(kb PRIVATE -Wall -Wextra)
    # BM25's logarithm. Part of the C standard library; some systems keep it
    # in a separate object.
    target_link_libraries(kb PRIVATE m)
endif()

# ---- tests -----------------------------------------------------------------
enable_testing()

file(GLOB KB_UNIT_SOURCES tests/unit/*.c)
add_executable(kb-unit-tests ${KB_LIB_SOURCES} ${KB_UNIT_SOURCES})
target_include_directories(kb-unit-tests PRIVATE src)
if(MSVC)
    target_compile_options(kb-unit-tests PRIVATE /W4)
else()
    target_compile_options(kb-unit-tests PRIVATE -Wall -Wextra)
    target_link_libraries(kb-unit-tests PRIVATE m)
endif()

add_test(NAME kb.unit COMMAND kb-unit-tests)
if(UNIX)
    add_test(NAME kb.e2e
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run.sh)
    set_tests_properties(kb.e2e PROPERTIES
        ENVIRONMENT "KB=$<TARGET_FILE:kb>;KB_TESTS=$<TARGET_FILE:kb-unit-tests>")
endif()

# ---- install / uninstall ---------------------------------------------------
include(GNUInstallDirs)
install(TARGETS kb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# CMake has no built-in uninstall; remove everything recorded in this build's
# install_manifest.txt.
if(NOT TARGET uninstall)
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND}
                -DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake)
endif()
