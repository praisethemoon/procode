#include "platform.h"
#include "rec.h"
#include "test.h"

void test_rec(void) {
    Arena *a = arena_new(0);
    char err[256];

    t_begin("rec: commit record encode/decode round-trip");
    Str oldt[2] = {{"foo(x);", 7}, {"bar();", 6}};
    Str newt[3] = {{"foo(x, y);", 10}, {"baz();", 6}, {"qux();", 6}};
    Rec rec;
    memset(&rec, 0, sizeof rec);
    rec.type = REC_COMMIT;
    rec.id = "L7";
    rec.user = "test bot \"quoted\"";
    rec.session = "S2";
    rec.file = "src/a b/weird\"name.c";
    rec.op = "edit";
    rec.msg = "multi\nline \"message\"\twith tabs";
    rec.ts = "2026-09-20T10:00:00Z";
    rec.prev = LAP_HASH_ZERO;
    rec.old_start = 10;
    rec.old_lines = 2;
    rec.new_start = 10;
    rec.new_lines = 3;
    rec.old_text = oldt;
    rec.old_n = 2;
    rec.new_text = newt;
    rec.new_n = 3;
    rec.eof_nl = false;
    size_t len;
    char *line = rec_encode(a, &rec, &len);
    ASSERT_TRUE(line != NULL && len > 0);

    Rec back;
    ASSERT_TRUE(rec_decode(a, line, len, &back, err, sizeof err));
    ASSERT_TRUE(back.type == REC_COMMIT);
    ASSERT_EQ_S(back.id, "L7");
    ASSERT_EQ_S(back.user, "test bot \"quoted\"");
    ASSERT_EQ_S(back.session, "S2");
    ASSERT_EQ_S(back.file, "src/a b/weird\"name.c");
    ASSERT_EQ_S(back.op, "edit");
    ASSERT_EQ_S(back.msg, "multi\nline \"message\"\twith tabs");
    ASSERT_EQ_I(back.old_start, 10);
    ASSERT_EQ_I(back.old_lines, 2);
    ASSERT_EQ_I(back.new_start, 10);
    ASSERT_EQ_I(back.new_lines, 3);
    ASSERT_TRUE(!back.eof_nl);
    ASSERT_EQ_I(back.old_n, 2);
    ASSERT_EQ_I(back.new_n, 3);
    ASSERT_TRUE(str_eq(back.new_text[0], newt[0]));
    ASSERT_EQ_S(back.hash, rec.hash);

    t_begin("rec: null session survives the round-trip");
    rec.session = NULL;
    line = rec_encode(a, &rec, &len);
    ASSERT_TRUE(rec_decode(a, line, len, &back, err, sizeof err));
    ASSERT_TRUE(back.session == NULL);

    t_begin("rec: missing user (pre-user logs) decodes as NULL");
    rec.user = NULL;
    line = rec_encode(a, &rec, &len);
    ASSERT_TRUE(rec_decode(a, line, len, &back, err, sizeof err));
    ASSERT_TRUE(back.user == NULL);

    t_begin("rec: session records round-trip");
    Rec s;
    memset(&s, 0, sizeof s);
    s.type = REC_SESSION_START;
    s.id = "S9";
    s.user = "session-bot";
    s.msg = "fix the flaky test";
    s.ts = "2026-09-20T10:00:00Z";
    s.prev = LAP_HASH_ZERO;
    line = rec_encode(a, &s, &len);
    ASSERT_TRUE(rec_decode(a, line, len, &back, err, sizeof err));
    ASSERT_TRUE(back.type == REC_SESSION_START);
    ASSERT_EQ_S(back.id, "S9");
    ASSERT_EQ_S(back.user, "session-bot");
    ASSERT_EQ_S(back.msg, "fix the flaky test");
    ASSERT_EQ_I(back.meta_n, 0);
    /* Always an object on a new record, even an empty one. */
    ASSERT_TRUE(strstr(line, "\"meta\":{}") != NULL);

    t_begin("rec: session metadata round-trips");
    const char *mk[] = {"ticket", "n", "ok"};
    const char *mv[] = {"\"T-12\"", "1", "true"};
    s.meta_keys = mk;
    s.meta_vals = mv;
    s.meta_n = 3;
    line = rec_encode(a, &s, &len);
    ASSERT_TRUE(strstr(line, "\"meta\":{\"ticket\":\"T-12\",\"n\":1,"
                             "\"ok\":true}") != NULL);
    ASSERT_TRUE(rec_decode(a, line, len, &back, err, sizeof err));
    ASSERT_EQ_I(back.meta_n, 3);
    ASSERT_EQ_S(rec_meta(&back, "ticket"), "\"T-12\"");
    ASSERT_EQ_S(rec_meta(&back, "n"), "1");
    ASSERT_EQ_S(rec_meta(&back, "ok"), "true");
    ASSERT_TRUE(rec_meta(&back, "milestone") == NULL);
    ASSERT_EQ_S(back.hash, s.hash);

    t_begin("rec: a command-line value is typed the way JSON would read it");
    const char *key, *val;
    ASSERT_TRUE(rec_meta_parse(a, "xyz=1", &key, &val, err, sizeof err));
    ASSERT_EQ_S(key, "xyz");
    ASSERT_EQ_S(val, "1");
    ASSERT_TRUE(rec_meta_parse(a, "x=-2.5e3", &key, &val, err, sizeof err));
    ASSERT_EQ_S(val, "-2.5e3");
    ASSERT_TRUE(rec_meta_parse(a, "x=false", &key, &val, err, sizeof err));
    ASSERT_EQ_S(val, "false");
    ASSERT_TRUE(rec_meta_parse(a, "x=007", &key, &val, err, sizeof err));
    ASSERT_EQ_S(val, "\"007\"");
    ASSERT_TRUE(rec_meta_parse(a, "ticket=T-12", &key, &val, err, sizeof err));
    ASSERT_EQ_S(val, "\"T-12\"");
    ASSERT_TRUE(rec_meta_parse(a, "empty=", &key, &val, err, sizeof err));
    ASSERT_EQ_S(val, "\"\"");
    ASSERT_TRUE(rec_meta_parse(a, "_A9=x", &key, &val, err, sizeof err));
    ASSERT_TRUE(!rec_meta_parse(a, "9a=x", &key, &val, err, sizeof err));
    ASSERT_TRUE(!rec_meta_parse(a, "a-b=x", &key, &val, err, sizeof err));
    ASSERT_TRUE(!rec_meta_parse(a, "=x", &key, &val, err, sizeof err));
    ASSERT_TRUE(!rec_meta_parse(a, "novalue", &key, &val, err, sizeof err));
    const char *bad_meta = "{\"type\":\"session_start\",\"id\":\"S1\","
                           "\"msg\":\"m\",\"meta\":{\"k\":[1]},\"ts\":\"t\","
                           "\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, bad_meta, strlen(bad_meta), &back, err,
                            sizeof err));

    t_begin("rec: malformed records are rejected");
    ASSERT_TRUE(!rec_decode(a, "{}", 2, &back, err, sizeof err));
    ASSERT_TRUE(!rec_decode(a, "{\"type\":\"bogus\",\"ts\":\"t\",\"prev\":"
                               "\"p\"}",
                            37, &back, err, sizeof err));
    const char *no_msg = "{\"type\":\"session_start\",\"id\":\"S1\","
                         "\"ts\":\"t\",\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, no_msg, strlen(no_msg), &back, err,
                            sizeof err));
    const char *bad_counts =
        "{\"type\":\"commit\",\"id\":\"L1\",\"session\":null,\"file\":\"f\","
        "\"op\":\"edit\",\"old_start\":1,\"old_lines\":2,\"new_start\":1,"
        "\"new_lines\":0,\"eof_nl\":true,\"old_text\":[\"only-one\"],"
        "\"new_text\":[],\"msg\":\"m\",\"ts\":\"t\",\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, bad_counts, strlen(bad_counts), &back, err,
                            sizeof err));

    t_begin("rec: torn trailing line is dropped and reported, chain intact");
    Rec ra, rb;
    memset(&ra, 0, sizeof ra);
    ra.type = REC_SESSION_START;
    ra.id = "S1";
    ra.msg = "torn tail test";
    ra.ts = "2026-09-21T00:00:00Z";
    ra.prev = LAP_HASH_ZERO;
    size_t la;
    char *l1 = rec_encode(a, &ra, &la);
    memset(&rb, 0, sizeof rb);
    rb.type = REC_SESSION_END;
    rb.id = "S1";
    rb.ts = "2026-09-21T00:00:01Z";
    rb.prev = ra.hash;
    size_t lb;
    char *l2 = rec_encode(a, &rb, &lb);
    StrBuf fsb;
    sb_init(&fsb, a);
    sb_putn(&fsb, l1, la);
    sb_putc(&fsb, '\n');
    sb_putn(&fsb, l2, lb);
    sb_putc(&fsb, '\n');
    sb_puts(&fsb, "{\"type\":\"commit\",\"id\":\"L9"); /* torn, no newline */
    /* cwd, not bin/: ctest runs from the build directory, where bin/ may
     * not exist, and a write that silently fails takes the assertions with
     * it */
    const char *tp = ".torn_unit_test.jsonl";
    ASSERT_TRUE(plat_write_file_atomic(tp, fsb.data, fsb.len));
    RecLog tl;
    char terr[256];
    ASSERT_TRUE(rec_log_load(a, tp, &tl, terr, sizeof terr));
    ASSERT_EQ_I(tl.count, 2);
    ASSERT_TRUE(tl.torn_tail);
    ASSERT_TRUE(tl.torn_bytes > 0);
    ASSERT_TRUE(tl.chain_ok);
    plat_remove_file(tp);

    t_begin("rec: tampering changes the hash (chain detection)");
    Rec r1;
    memset(&r1, 0, sizeof r1);
    r1.type = REC_SESSION_START;
    r1.id = "S1";
    r1.msg = "original";
    r1.ts = "2026-09-20T10:00:00Z";
    r1.prev = LAP_HASH_ZERO;
    char *tline = rec_encode(a, &r1, &len);
    char *tampered = arena_strdup(a, tline);
    /* flip one byte inside the message */
    char *pos = strstr(tampered, "original");
    ASSERT_TRUE(pos != NULL);
    pos[0] = 'O';
    Rec back2;
    ASSERT_TRUE(rec_decode(a, tampered, len, &back2, err, sizeof err));
    ASSERT_TRUE(strcmp(back2.hash, r1.hash) != 0);

    arena_free(a);
}
