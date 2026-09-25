#include "json.h"
#include "test.h"

void test_json(void) {
    Arena *a = arena_new(0);
    char err[256];

    t_begin("json: scalars");
    JVal *v = json_parse(a, "42", 2, err, sizeof err);
    ASSERT_TRUE(v && v->t == J_NUM && v->is_int);
    ASSERT_EQ_I(v->i, 42);
    v = json_parse(a, "-3.5", 4, err, sizeof err);
    ASSERT_TRUE(v && v->t == J_NUM && !v->is_int);
    v = json_parse(a, "true", 4, err, sizeof err);
    ASSERT_TRUE(v && v->t == J_BOOL && v->b);
    v = json_parse(a, "null", 4, err, sizeof err);
    ASSERT_TRUE(v && v->t == J_NULL);
    v = json_parse(a, "\"hi\"", 4, err, sizeof err);
    ASSERT_TRUE(v && v->t == J_STR);
    ASSERT_EQ_S(v->s.ptr, "hi");

    t_begin("json: object and array access");
    const char *doc =
        "{\"id\":\"L7\",\"n\":3,\"ok\":true,\"none\":null,"
        "\"arr\":[\"a\",\"b\"],\"nested\":{\"x\":-1}}";
    v = json_parse(a, doc, strlen(doc), err, sizeof err);
    ASSERT_TRUE(v && v->t == J_OBJ);
    ASSERT_EQ_S(jobj_str(v, "id"), "L7");
    ASSERT_EQ_I(jobj_int(v, "n", -1), 3);
    ASSERT_TRUE(jobj_bool(v, "ok", false));
    ASSERT_TRUE(jobj_get(v, "none") && jobj_get(v, "none")->t == J_NULL);
    ASSERT_TRUE(jobj_str(v, "missing") == NULL);
    JVal *arr = jobj_get(v, "arr");
    ASSERT_TRUE(arr && arr->t == J_ARR);
    ASSERT_EQ_I(arr->arr.n, 2);
    ASSERT_EQ_S(arr->arr.items[1]->s.ptr, "b");
    ASSERT_EQ_I(jobj_int(jobj_get(v, "nested"), "x", 0), -1);

    t_begin("json: string escapes decode");
    const char *esc = "\"a\\n\\t\\\"\\\\b\\u0041\\u00e9\"";
    v = json_parse(a, esc, strlen(esc), err, sizeof err);
    ASSERT_TRUE(v && v->t == J_STR);
    ASSERT_EQ_S(v->s.ptr, "a\n\t\"\\bA\xc3\xa9");

    t_begin("json: surrogate pair decodes to UTF-8");
    const char *emoji = "\"\\ud83d\\ude00\"";
    v = json_parse(a, emoji, strlen(emoji), err, sizeof err);
    ASSERT_TRUE(v != NULL);
    ASSERT_EQ_S(v->s.ptr, "\xf0\x9f\x98\x80");

    t_begin("json: malformed input is rejected");
    const char *bad[] = {
        "",            "{",           "{\"a\":}",   "[1,]",
        "\"unclosed",  "{\"a\" 1}",   "nul",        "01x",
        "\"\\q\"",     "\"\\ud800\"", "1 2",        "{\"a\":1}extra",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        JVal *b = json_parse(a, bad[i], strlen(bad[i]), err, sizeof err);
        ASSERT_TRUE(b == NULL);
    }

    t_begin("json: raw control characters in strings are rejected");
    const char raw[] = {'"', 'a', 0x01, '"'};
    ASSERT_TRUE(json_parse(a, raw, sizeof raw, err, sizeof err) == NULL);

    t_begin("json: escape emitter round-trips");
    const char *tricky = "line1\nline2\t\"quoted\" \\slash\\ \x01end";
    StrBuf sb;
    sb_init(&sb, a);
    json_escape_c(&sb, tricky);
    char *emitted = sb_finish(&sb);
    v = json_parse(a, emitted, sb.len, err, sizeof err);
    ASSERT_TRUE(v && v->t == J_STR);
    ASSERT_EQ_S(v->s.ptr, tricky);

    t_begin("json: deep nesting is bounded, not a stack overflow");
    StrBuf deep;
    sb_init(&deep, a);
    for (int32_t i = 0; i < 200; i++)
        sb_putc(&deep, '[');
    for (int32_t i = 0; i < 200; i++)
        sb_putc(&deep, ']');
    ASSERT_TRUE(json_parse(a, deep.data, deep.len, err, sizeof err) == NULL);

    arena_free(a);
}
