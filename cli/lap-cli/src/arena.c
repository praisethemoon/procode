#include "arena.h"

#include <stdarg.h>

typedef struct Chunk {
    struct Chunk *next;
    size_t cap;
    size_t used;
    size_t pad_; /* keeps sizeof(Chunk) a multiple of 16 so the data area
                    right after the header is 16-aligned */
    /* data follows the header */
} Chunk;

struct Arena {
    Chunk *head;
    size_t min_chunk;
    size_t total_used;
};

#define ARENA_ALIGN 16

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "lap: out of memory (%zu bytes)\n", n);
        exit(LAP_EXIT_FATAL);
    }
    return p;
}

static Chunk *chunk_new(size_t cap) {
    Chunk *c = (Chunk *)xmalloc(sizeof(Chunk) + cap);
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

Arena *arena_new(size_t initial_chunk) {
    if (initial_chunk < 4096)
        initial_chunk = 4096;
    Arena *a = (Arena *)xmalloc(sizeof(Arena));
    a->min_chunk = initial_chunk;
    a->head = chunk_new(initial_chunk);
    a->total_used = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size) {
    if (size == 0)
        size = 1;
    size = (size + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    Chunk *c = a->head;
    if (c->cap - c->used < size) {
        size_t want = c->cap * 2;
        if (want < size)
            want = size;
        Chunk *n = chunk_new(want);
        n->next = a->head;
        a->head = n;
        c = n;
    }
    void *p = (char *)(c + 1) + c->used;
    c->used += size;
    a->total_used += size;
    return p;
}

void *arena_alloc0(Arena *a, size_t size) {
    void *p = arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

void *arena_realloc(Arena *a, void *old, size_t old_size, size_t new_size) {
    if (new_size <= old_size && old)
        return old;
    void *p = arena_alloc(a, new_size);
    if (old && old_size)
        memcpy(p, old, old_size);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    return arena_strndup(a, s, strlen(s));
}

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return arena_strdup(a, "");
    }
    char *p = (char *)arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

size_t arena_used(const Arena *a) {
    return a->total_used;
}

void arena_free(Arena *a) {
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        free(c);
        c = next;
    }
    free(a);
}
