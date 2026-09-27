/* Arena (region) allocator. One arena per command invocation: bump-allocate
 * everything, free it all at exit. No individual frees. A loop over many
 * items (status's files) does each item's work in a scratch arena it resets
 * between items, so memory follows the largest item, not their sum.
 */
#ifndef LAP_ARENA_H
#define LAP_ARENA_H

#include "lap.h"

typedef struct Arena Arena;

Arena *arena_new(size_t initial_chunk);
void *arena_alloc(Arena *a, size_t size);
void *arena_alloc0(Arena *a, size_t size);
/* Grow-by-copy realloc. old may be NULL (acts as alloc). */
void *arena_realloc(Arena *a, void *old, size_t old_size, size_t new_size);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_printf(Arena *a, const char *fmt, ...);
size_t arena_used(const Arena *a);
void arena_free(Arena *a);
/* Empties the arena for reuse: every allocation from it is gone, and only
 * its largest chunk is kept. For work done one item at a time in a loop. */
void arena_reset(Arena *a);

/* Growable-array helper: ensures capacity for one more element. */
#define ARENA_GROW(a, arr, n, cap, T)                                          \
    do {                                                                       \
        if ((n) == (cap)) {                                                    \
            size_t grow_nc_ = (cap) ? (cap) * 2 : 8;                           \
            (arr) = (T *)arena_realloc((a), (arr), (cap) * sizeof(T),          \
                                       grow_nc_ * sizeof(T));                  \
            (cap) = grow_nc_;                                                  \
        }                                                                      \
    } while (0)

#endif /* LAP_ARENA_H */
