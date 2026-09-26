#include "errdet.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The fields, comma-separated, without the braces; `out` is the object as
 * handed out. */
static char fields[2048];
static size_t used;
static char out[sizeof fields + 3];
static char owner[32];

void errdet_begin(const char *code) {
    used = 0;
    fields[0] = '\0';
    snprintf(owner, sizeof owner, "%s", code);
}

typedef struct {
    char buf[sizeof fields];
    size_t n;
    bool full;
} Field;

static void put(Field *f, const char *s, size_t len) {
    if (f->full || f->n + len >= sizeof f->buf) {
        f->full = true;
        return;
    }
    memcpy(f->buf + f->n, s, len);
    f->n += len;
}

static void put_string(Field *f, const char *s) {
    put(f, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char)*p;
            put(f, esc, 2);
        } else if (*p < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", *p);
            put(f, esc, 6);
        } else {
            put(f, (const char *)p, 1);
        }
    }
    put(f, "\"", 1);
}

static void begin(Field *f, const char *key) {
    f->n = 0;
    f->full = false;
    if (used > 0)
        put(f, ",", 1);
    put_string(f, key);
    put(f, ":", 1);
}

static void commit(Field *f) {
    if (f->full || used + f->n >= sizeof fields)
        return;
    memcpy(fields + used, f->buf, f->n);
    used += f->n;
    fields[used] = '\0';
}

void errdet_str(const char *key, const char *value) {
    static Field f;
    begin(&f, key);
    put_string(&f, value);
    commit(&f);
}

void errdet_int(const char *key, int64_t value) {
    static Field f;
    begin(&f, key);
    char num[32];
    int len = snprintf(num, sizeof num, "%" PRId64, value);
    put(&f, num, (size_t)len);
    commit(&f);
}

void errdet_strs(const char *key, const char *const *values, size_t n) {
    static Field f;
    begin(&f, key);
    put(&f, "[", 1);
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            put(&f, ",", 1);
        put_string(&f, values[i]);
    }
    put(&f, "]", 1);
    commit(&f);
}

const char *errdet_json(const char *code) {
    if (used == 0 || strcmp(code, owner) != 0)
        return NULL;
    snprintf(out, sizeof out, "{%s}", fields);
    return out;
}
