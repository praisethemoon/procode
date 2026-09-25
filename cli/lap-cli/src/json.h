/* Minimal JSON: a DOM parser and escape-correct emit helpers.
 * Only what lap's own schema needs, but robust against malformed input.
 */
#ifndef LAP_JSON_H
#define LAP_JSON_H

#include "str.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;

struct JVal {
    JType t;
    bool b;         /* J_BOOL */
    double num;     /* J_NUM */
    int64_t i;      /* J_NUM when integral */
    bool is_int;
    Str s;          /* J_STR: decoded bytes, NUL-terminated */
    struct {
        JVal **items;
        size_t n;
    } arr;
    struct {
        Str *keys;
        JVal **vals;
        size_t n;
    } obj;
};

/* Parses exactly one JSON value; trailing non-whitespace is an error.
 * Returns NULL on failure with a message in err.
 */
JVal *json_parse(Arena *a, const char *data, size_t len, char *err,
                 size_t errsz);

/* Object lookup helpers; return NULL / default when missing or wrong type. */
JVal *jobj_get(const JVal *obj, const char *key);
const char *jobj_str(const JVal *obj, const char *key); /* NULL if absent */
int64_t jobj_int(const JVal *obj, const char *key, int64_t def);
bool jobj_bool(const JVal *obj, const char *key, bool def);

/* Emits s as a JSON string, including surrounding quotes. */
void json_escape(StrBuf *sb, const char *s, size_t len);
void json_escape_c(StrBuf *sb, const char *s);

#endif /* LAP_JSON_H */
