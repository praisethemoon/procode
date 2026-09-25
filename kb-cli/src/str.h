/* String views, growable string builder, line splitting.
 * str_find is the single seam for substring search: swap in a SIMD
 * implementation (e.g. stringzilla) here later without touching callers.
 */
#ifndef KB_STR_H
#define KB_STR_H

#include "arena.h"

typedef struct {
    const char *ptr;
    size_t len;
} Str;

Str str_c(const char *s);
Str str_n(const char *s, size_t n);
bool str_eq(Str a, Str b);
bool str_eq_c(Str a, const char *b);
/* Byte offset of first occurrence of needle in hay, or -1. */
int64_t str_find(Str hay, Str needle);
char *str_dup_c(Arena *a, Str s); /* NUL-terminated copy */
uint64_t str_hash(Str s);         /* FNV-1a 64 */

/* How many bytes at the end of s[0..n) begin a UTF-8 character without
 * finishing it; 0 when the tail is whole.
 *
 * Everything here cuts text on byte boundaries — a chunk span, a snippet
 * window, a heading clamped to a length — and any of those can land inside
 * a character. A JSON string is defined over text rather than bytes, so
 * half a character in one field can cost a caller the whole response. Every
 * place that truncates text for display subtracts this first. */
size_t utf8_dangling(const char *s, size_t n);

typedef struct {
    Arena *a;
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb, Arena *a);
void sb_putn(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_putc(StrBuf *sb, char c);
void sb_printf(StrBuf *sb, const char *fmt, ...);
char *sb_finish(StrBuf *sb); /* NUL-terminates, returns buffer */

/* A file's content as lines. Line content excludes the trailing '\n' but keeps
 * any '\r' (CRLF files stay byte-faithful). eof_nl records whether the file
 * ends with a newline; an empty file has 0 lines and eof_nl = true.
 */
typedef struct {
    Str *lines;
    int32_t count;
    bool eof_nl;
} Lines;

Lines split_lines(Arena *a, const char *data, size_t len);
/* Inverse of split_lines. */
char *join_lines(Arena *a, Lines l, size_t *out_len);

/* Open-addressing string set; arena-backed, grows by rehash. */
typedef struct {
    Arena *a;
    const char **slots;
    size_t cap, n;
} StrSet;

void strset_init(StrSet *s, Arena *a);
/* Returns true if key was newly added, false if already present. */
bool strset_add(StrSet *s, const char *key);
bool strset_has(const StrSet *s, const char *key);

#endif /* KB_STR_H */
