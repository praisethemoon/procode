/* String views, growable string builder, line splitting.
 * str_find is the single seam for substring search: swap in a SIMD
 * implementation (e.g. stringzilla) here later without touching callers.
 */
#ifndef LAP_STR_H
#define LAP_STR_H

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
/* l with the '\r' of every CRLF line ending dropped: lap reads CRLF as LF
 * (SPEC, Edit detection). A last line that no '\n' ends keeps its '\r'.
 * The line count is unchanged, so regions found on this copy are regions
 * of l. */
Lines lines_without_cr(Arena *a, Lines l);

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

#endif /* LAP_STR_H */
