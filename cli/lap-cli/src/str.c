#include "str.h"

#include <stdarg.h>

Str str_c(const char *s) {
    Str r = {s, s ? strlen(s) : 0};
    return r;
}

Str str_n(const char *s, size_t n) {
    Str r = {s, n};
    return r;
}

bool str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

bool str_eq_c(Str a, const char *b) {
    return str_eq(a, str_c(b));
}

int64_t str_find(Str hay, Str needle) {
    if (needle.len == 0)
        return 0;
    if (needle.len > hay.len)
        return -1;
    const char *p = hay.ptr;
    const char *end = hay.ptr + hay.len - needle.len + 1;
    while (p < end) {
        const char *hit =
            (const char *)memchr(p, needle.ptr[0], (size_t)(end - p));
        if (!hit)
            return -1;
        if (memcmp(hit, needle.ptr, needle.len) == 0)
            return (int64_t)(hit - hay.ptr);
        p = hit + 1;
    }
    return -1;
}

char *str_dup_c(Arena *a, Str s) {
    return arena_strndup(a, s.ptr ? s.ptr : "", s.len);
}

/* FNV-1a 64. Constants written in hex so they match the published spec on
 * sight: a mistyped decimal basis still hashes well and passes every
 * dispersion test, so nothing would ever catch it. */
uint64_t str_hash(Str s) {
    uint64_t h = 0xcbf29ce484222325ULL; /* offset basis */
    for (size_t i = 0; i < s.len; i++) {
        h ^= (unsigned char)s.ptr[i];
        h *= 0x100000001b3ULL; /* prime */
    }
    return h;
}

void sb_init(StrBuf *sb, Arena *a) {
    sb->a = a;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_reserve(StrBuf *sb, size_t extra) {
    if (extra > (size_t)-1 - sb->len - 1) {
        /* overflow guard; unreachable behind the 64 MB input cap */
        fprintf(stderr, "lap: string builder overflow\n");
        exit(LAP_EXIT_FATAL);
    }
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return;
    size_t nc = sb->cap ? sb->cap * 2 : 64;
    while (nc < need) {
        if (nc > (size_t)-1 / 2) {
            nc = need;
            break;
        }
        nc *= 2;
    }
    sb->data = (char *)arena_realloc(sb->a, sb->data, sb->cap, nc);
    sb->cap = nc;
}

void sb_putn(StrBuf *sb, const char *s, size_t n) {
    if (n == 0)
        return; /* also avoids memcpy(_, NULL, 0), which is UB */
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
}

void sb_puts(StrBuf *sb, const char *s) {
    sb_putn(sb, s, strlen(s));
}

void sb_putc(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    sb->len += (size_t)n;
    va_end(ap2);
}

char *sb_finish(StrBuf *sb) {
    sb_reserve(sb, 0);
    sb->data[sb->len] = '\0';
    return sb->data;
}

void strset_init(StrSet *s, Arena *a) {
    s->a = a;
    s->cap = 64;
    s->n = 0;
    s->slots = (const char **)arena_alloc0(a, s->cap * sizeof(char *));
}

static size_t strset_slot(const char **slots, size_t cap, const char *key) {
    size_t i = (size_t)str_hash(str_c(key)) & (cap - 1);
    while (slots[i] && strcmp(slots[i], key) != 0)
        i = (i + 1) & (cap - 1);
    return i;
}

bool strset_has(const StrSet *s, const char *key) {
    return s->slots[strset_slot(s->slots, s->cap, key)] != NULL;
}

bool strset_add(StrSet *s, const char *key) {
    if (s->n * 2 >= s->cap) {
        size_t ncap = s->cap * 2;
        const char **ns =
            (const char **)arena_alloc0(s->a, ncap * sizeof(char *));
        for (size_t i = 0; i < s->cap; i++) {
            if (s->slots[i])
                ns[strset_slot(ns, ncap, s->slots[i])] = s->slots[i];
        }
        s->slots = ns;
        s->cap = ncap;
    }
    size_t i = strset_slot(s->slots, s->cap, key);
    if (s->slots[i])
        return false;
    s->slots[i] = arena_strdup(s->a, key);
    s->n++;
    return true;
}

Lines split_lines(Arena *a, const char *data, size_t len) {
    Lines l;
    l.lines = NULL;
    l.count = 0;
    l.eof_nl = true;
    if (len == 0)
        return l;

    size_t cap = 0, n = 0;
    Str *arr = NULL;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            ARENA_GROW(a, arr, n, cap, Str);
            arr[n++] = str_n(data + start, i - start);
            start = i + 1;
        }
    }
    if (start < len) {
        ARENA_GROW(a, arr, n, cap, Str);
        arr[n++] = str_n(data + start, len - start);
        l.eof_nl = false;
    }
    l.lines = arr;
    l.count = (int32_t)n;
    return l;
}

char *join_lines(Arena *a, Lines l, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 0; i < l.count; i++) {
        sb_putn(&sb, l.lines[i].ptr, l.lines[i].len);
        if (i + 1 < l.count || l.eof_nl)
            sb_putc(&sb, '\n');
    }
    if (out_len)
        *out_len = sb.len;
    return sb_finish(&sb);
}
