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

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# utf8proc is compiled into kb, not linked from a DLL: without this its header
# declares every function dllimport on Windows, and MSVC refuses to define them.
add_compile_definitions(UTF8PROC_STATIC)

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
    src/gguf.c
    src/quant.c
    src/wpm.c
    src/bpe.c
    src/gitignore.c
    src/dirscan.c
    src/syntax.c
    src/imports.c
    src/syntax_tags.c
    src/third_party/utf8proc/utf8proc.c
    src/embed.c
    src/modelrec.c
    src/vectors.c
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
    src/help.c
)

# tree-sitter and its grammars, vendored (src/third_party/TREE-SITTER.md).
set(KB_TS_SOURCES
    src/third_party/tree-sitter/lib/src/lib.c
    src/third_party/tree-sitter-c/src/parser.c
    src/third_party/tree-sitter-typescript/typescript/src/parser.c
    src/third_party/tree-sitter-typescript/typescript/src/scanner.c
    src/third_party/tree-sitter-typescript/tsx/src/parser.c
    src/third_party/tree-sitter-typescript/tsx/src/scanner.c
    src/third_party/tree-sitter-javascript/src/parser.c
    src/third_party/tree-sitter-javascript/src/scanner.c
    src/third_party/tree-sitter-python/src/parser.c
    src/third_party/tree-sitter-python/src/scanner.c
    src/third_party/tree-sitter-go/src/parser.c
    src/third_party/tree-sitter-rust/src/parser.c
    src/third_party/tree-sitter-rust/src/scanner.c
    src/third_party/tree-sitter-asm/src/parser.c
)
list(APPEND KB_LIB_SOURCES ${KB_TS_SOURCES})

# tree_sitter/api.h, for src/syntax.c and the runtime itself.
include_directories(src/third_party/tree-sitter/lib/include)
# The vendored sources are someone else's code: their warnings are not kb's.
set_source_files_properties(${KB_TS_SOURCES} PROPERTIES
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
# tree-sitter's lib.c includes "unicode/umachine.h" and its siblings from
# lib/src, as its own build does. Without this gcc and clang fall back to the
# system's ICU headers: Apple's on macOS, none on a Linux without libicu-dev.
set_source_files_properties(src/third_party/tree-sitter/lib/src/lib.c PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/tree-sitter/lib/src)
# The runtime does not keep to C's aliasing rules: gcc's -O3 on aarch64 (the
# linux-arm64 release build) turns that into a crash on the first parse.
# -O1, -O2, clang, and every other target happened to be fine.
if(NOT MSVC)
    set_property(SOURCE src/third_party/tree-sitter/lib/src/lib.c APPEND
        PROPERTY COMPILE_OPTIONS -fno-strict-aliasing)
endif()

add_executable(kb ${KB_LIB_SOURCES} src/main.c)
# Embedding runs its matrix products on every core (plat_parallel).
target_link_libraries(kb PRIVATE Threads::Threads)

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
target_link_libraries(kb-unit-tests PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(kb-unit-tests PRIVATE /W4)
else()
    target_compile_options(kb-unit-tests PRIVATE -Wall -Wextra)
    target_link_libraries(kb-unit-tests PRIVATE m)
endif()

# The unit tests read their fixtures relative to this directory, as under
# `make test`; ctest would otherwise run them from the build tree.
add_test(NAME kb.unit COMMAND kb-unit-tests
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
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
