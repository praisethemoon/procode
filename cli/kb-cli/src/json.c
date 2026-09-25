#include "json.h"

typedef struct {
    Arena *a;
    const char *start;
    const char *p;
    const char *end;
    char *err;
    size_t errsz;
    bool failed;
    int depth;
} Parser;

#define JSON_MAX_DEPTH 128

static void fail(Parser *ps, const char *msg) {
    if (!ps->failed && ps->err && ps->errsz)
        snprintf(ps->err, ps->errsz, "%s (at byte offset %ld)", msg,
                 (long)(ps->p - ps->start));
    ps->failed = true;
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
            *ps->p == '\r'))
        ps->p++;
}

static JVal *jnew(Parser *ps, JType t) {
    JVal *v = (JVal *)arena_alloc0(ps->a, sizeof(JVal));
    v->t = t;
    return v;
}

static JVal *parse_value(Parser *ps);

static bool utf8_put(StrBuf *sb, uint32_t cp) {
    if (cp <= 0x7f) {
        sb_putc(sb, (char)cp);
    } else if (cp <= 0x7ff) {
        sb_putc(sb, (char)(0xc0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        sb_putc(sb, (char)(0xe0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3f)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0x10ffff) {
        sb_putc(sb, (char)(0xf0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3f)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3f)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3f)));
    } else {
        return false;
    }
    return true;
}

static int hex4(Parser *ps, uint32_t *out) {
    if (ps->end - ps->p < 4)
        return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
        else
            return -1;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

/* Parses a string literal (cursor on opening quote); returns decoded bytes. */
static bool parse_string_raw(Parser *ps, Str *out) {
    if (ps->p >= ps->end || *ps->p != '"') {
        fail(ps, "expected string");
        return false;
    }
    ps->p++;
    StrBuf sb;
    sb_init(&sb, ps->a);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            size_t n = sb.len;
            out->ptr = sb_finish(&sb);
            out->len = n;
            return true;
        }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) {
                fail(ps, "unterminated escape");
                return false;
            }
            char e = *ps->p++;
            switch (e) {
            case '"': sb_putc(&sb, '"'); break;
            case '\\': sb_putc(&sb, '\\'); break;
            case '/': sb_putc(&sb, '/'); break;
            case 'b': sb_putc(&sb, '\b'); break;
            case 'f': sb_putc(&sb, '\f'); break;
            case 'n': sb_putc(&sb, '\n'); break;
            case 'r': sb_putc(&sb, '\r'); break;
            case 't': sb_putc(&sb, '\t'); break;
            case 'u': {
                uint32_t cp;
                if (hex4(ps, &cp) != 0) {
                    fail(ps, "bad \\u escape");
                    return false;
                }
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    /* high surrogate: expect \uDC00-\uDFFF next */
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' &&
                        ps->p[1] == 'u') {
                        ps->p += 2;
                        uint32_t lo;
                        if (hex4(ps, &lo) != 0 || lo < 0xdc00 || lo > 0xdfff) {
                            fail(ps, "bad surrogate pair");
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    } else {
                        fail(ps, "lone high surrogate");
                        return false;
                    }
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    fail(ps, "lone low surrogate");
                    return false;
                }
                if (!utf8_put(&sb, cp)) {
                    fail(ps, "invalid codepoint");
                    return false;
                }
                break;
            }
            default:
                fail(ps, "unknown escape");
                return false;
            }
            continue;
        }
        if (c < 0x20) {
            fail(ps, "raw control character in string");
            return false;
        }
        sb_putc(&sb, (char)c);
        ps->p++;
    }
    fail(ps, "unterminated string");
    return false;
}

static JVal *parse_number(Parser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    bool any = false;
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
        ps->p++;
        any = true;
    }
    bool integral = true;
    if (ps->p < ps->end && *ps->p == '.') {
        integral = false;
        ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        integral = false;
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
            ps->p++;
    }
    if (!any) {
        fail(ps, "bad number");
        return NULL;
    }
    char *tmp = arena_strndup(ps->a, start, (size_t)(ps->p - start));
    JVal *v = jnew(ps, J_NUM);
    v->num = strtod(tmp, NULL);
    if (integral) {
        v->i = (int64_t)strtoll(tmp, NULL, 10);
        v->is_int = true;
    } else {
        v->i = (int64_t)v->num;
        v->is_int = false;
    }
    return v;
}

static bool lit(Parser *ps, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, word, n) == 0) {
        ps->p += n;
        return true;
    }
    return false;
}

static JVal *parse_value_inner(Parser *ps);

/* EVERY VALUE REMEMBERS THE BYTES IT CAME FROM.
 *
 * `index-api.md` §1.2 makes a document's `meta` free-form: whatever the writer
 * handed in is what a reader gets back. That cannot be satisfied by parsing
 * and re-printing — a printer rewrites 1.0 as 1, reorders keys and respells
 * escapes, none of which the writer asked for, and the store would be handing
 * back something nobody wrote.
 *
 * So the span is recorded here rather than in each of the type parsers: this
 * wrapper brackets whatever `parse_value_inner` consumed, which means a value
 * nested three objects deep gets its span for free and no new parser can
 * forget to set one. Leading whitespace is skipped before the start is taken
 * so the span is the value, not the gap in front of it. */
static JVal *parse_value(Parser *ps) {
    skip_ws(ps);
    const char *from = ps->p;
    JVal *v = parse_value_inner(ps);
    if (v) {
        v->src.ptr = from;
        v->src.len = (size_t)(ps->p - from);
    }
    return v;
}

static JVal *parse_value_inner(Parser *ps) {
    if (ps->failed)
        return NULL;
    if (++ps->depth > JSON_MAX_DEPTH) {
        fail(ps, "nesting too deep");
        return NULL;
    }
    skip_ws(ps);
    if (ps->p >= ps->end) {
        fail(ps, "unexpected end of input");
        ps->depth--;
        return NULL;
    }
    JVal *v = NULL;
    char c = *ps->p;
    if (c == '{') {
        ps->p++;
        v = jnew(ps, J_OBJ);
        Str *keys = NULL;
        JVal **vals = NULL;
        size_t n = 0, kcap = 0, vcap = 0;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
        } else {
            for (;;) {
                skip_ws(ps);
                Str key;
                if (!parse_string_raw(ps, &key)) {
                    ps->depth--;
                    return NULL;
                }
                skip_ws(ps);
                if (ps->p >= ps->end || *ps->p != ':') {
                    fail(ps, "expected ':'");
                    ps->depth--;
                    return NULL;
                }
                ps->p++;
                JVal *val = parse_value(ps);
                if (!val) {
                    ps->depth--;
                    return NULL;
                }
                ARENA_GROW(ps->a, keys, n, kcap, Str);
                ARENA_GROW(ps->a, vals, n, vcap, JVal *);
                keys[n] = key;
                vals[n] = val;
                n++;
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') {
                    ps->p++;
                    continue;
                }
                if (ps->p < ps->end && *ps->p == '}') {
                    ps->p++;
                    break;
                }
                fail(ps, "expected ',' or '}'");
                ps->depth--;
                return NULL;
            }
        }
        v->obj.keys = keys;
        v->obj.vals = vals;
        v->obj.n = n;
    } else if (c == '[') {
        ps->p++;
        v = jnew(ps, J_ARR);
        JVal **items = NULL;
        size_t n = 0, cap = 0;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
        } else {
            for (;;) {
                JVal *item = parse_value(ps);
                if (!item) {
                    ps->depth--;
                    return NULL;
                }
                ARENA_GROW(ps->a, items, n, cap, JVal *);
                items[n++] = item;
                skip_ws(ps);
                if (ps->p < ps->end && *ps->p == ',') {
                    ps->p++;
                    continue;
                }
                if (ps->p < ps->end && *ps->p == ']') {
                    ps->p++;
                    break;
                }
                fail(ps, "expected ',' or ']'");
                ps->depth--;
                return NULL;
            }
        }
        v->arr.items = items;
        v->arr.n = n;
    } else if (c == '"') {
        v = jnew(ps, J_STR);
        if (!parse_string_raw(ps, &v->s)) {
            ps->depth--;
            return NULL;
        }
    } else if (c == 't') {
        if (!lit(ps, "true")) {
            fail(ps, "bad literal");
            ps->depth--;
            return NULL;
        }
        v = jnew(ps, J_BOOL);
        v->b = true;
    } else if (c == 'f') {
        if (!lit(ps, "false")) {
            fail(ps, "bad literal");
            ps->depth--;
            return NULL;
        }
        v = jnew(ps, J_BOOL);
        v->b = false;
    } else if (c == 'n') {
        if (!lit(ps, "null")) {
            fail(ps, "bad literal");
            ps->depth--;
            return NULL;
        }
        v = jnew(ps, J_NULL);
    } else {
        v = parse_number(ps);
    }
    ps->depth--;
    return ps->failed ? NULL : v;
}

JVal *json_parse(Arena *a, const char *data, size_t len, char *err,
                 size_t errsz) {
    Parser ps;
    ps.a = a;
    ps.start = data;
    ps.p = data;
    ps.end = data + len;
    ps.err = err;
    ps.errsz = errsz;
    ps.failed = false;
    ps.depth = 0;
    if (err && errsz)
        err[0] = '\0';
    JVal *v = parse_value(&ps);
    if (!v)
        return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        if (err && errsz)
            snprintf(err, errsz, "trailing garbage after JSON value");
        return NULL;
    }
    return v;
}

JVal *jobj_get(const JVal *obj, const char *key) {
    if (!obj || obj->t != J_OBJ)
        return NULL;
    Str k = str_c(key);
    for (size_t i = 0; i < obj->obj.n; i++) {
        if (str_eq(obj->obj.keys[i], k))
            return obj->obj.vals[i];
    }
    return NULL;
}

const char *jobj_str(const JVal *obj, const char *key) {
    JVal *v = jobj_get(obj, key);
    if (!v || v->t != J_STR)
        return NULL;
    return v->s.ptr;
}

int64_t jobj_int(const JVal *obj, const char *key, int64_t def) {
    JVal *v = jobj_get(obj, key);
    if (!v || v->t != J_NUM)
        return def;
    return v->is_int ? v->i : (int64_t)v->num;
}

bool jobj_bool(const JVal *obj, const char *key, bool def) {
    JVal *v = jobj_get(obj, key);
    if (!v || v->t != J_BOOL)
        return def;
    return v->b;
}

void json_escape(StrBuf *sb, const char *s, size_t len) {
    sb_putc(sb, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\b': sb_puts(sb, "\\b"); break;
        case '\f': sb_puts(sb, "\\f"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        default:
            if (c < 0x20)
                sb_printf(sb, "\\u%04x", (unsigned)c);
            else
                sb_putc(sb, (char)c);
        }
    }
    sb_putc(sb, '"');
}

void json_escape_c(StrBuf *sb, const char *s) {
    json_escape(sb, s, strlen(s));
}
