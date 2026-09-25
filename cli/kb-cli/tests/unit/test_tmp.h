/* Scratch directories for tests that need a real store on a real filesystem.
 *
 * The store is the thing under test in most of this suite, and it is defined
 * by what it puts on disk — a lock another process can see, an append that
 * survives a crash, a blob named by its own digest. None of that can be
 * exercised against an in-memory double without testing the double instead, so
 * these tests use the filesystem and clean up after themselves. */
#ifndef KB_TEST_TMP_H
#define KB_TEST_TMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../src/arena.h"
/* `test_tmp.c` walks and removes real directories, so it needs the platform
 * layer as well as the arena — this header is what carries both to it. */
#include "../../src/platform.h"

/* A unique, existing directory under the platform's temporary location.
 * Distinct per call, so two tests in one run cannot collide. */
void tmp_dir(char *out, size_t outsz);

/* Remove a directory and everything under it. Tests call this even when they
 * have failed, so it must tolerate a tree that is missing or half-built. */
void tmp_rm(Arena *a, const char *root);

/* How many regular files a directory holds, not recursing. Used to assert
 * that re-ingesting identical content writes no second blob. */
int32_t tmp_count_files(Arena *a, const char *dir);

/* Append bytes to a file exactly as given — no newline, no framing, no
 * escaping. This is how a torn write is planted: the tests need to produce the
 * unterminated tail a crashed writer leaves, which the store's own append can
 * never emit. */
bool tmp_append_raw(const char *path, const char *data, size_t len);

#endif
