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
    rec.intent = "multi\nline \"intent\"\twith tabs";
    rec.behavior = "what it \"does\"";
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
    ASSERT_EQ_S(back.intent, "multi\nline \"intent\"\twith tabs");
    ASSERT_EQ_S(back.behavior, "what it \"does\"");
    ASSERT_TRUE(!back.forced);
    ASSERT_TRUE(strstr(line, "\"forced\"") == NULL);
    ASSERT_TRUE(strstr(line, "\"msg\"") == NULL);
    ASSERT_EQ_I(back.old_start, 10);
    ASSERT_EQ_I(back.old_lines, 2);
    ASSERT_EQ_I(back.new_start, 10);
    ASSERT_EQ_I(back.new_lines, 3);
    ASSERT_TRUE(!back.eof_nl);
    ASSERT_EQ_I(back.old_n, 2);
    ASSERT_EQ_I(back.new_n, 3);
    ASSERT_TRUE(str_eq(back.new_text[0], newt[0]));
    ASSERT_EQ_S(back.hash, rec.hash);

    t_begin("rec: forced is written only when set, and read back");
    rec.forced = true;
    line = rec_encode(a, &rec, &len);
    ASSERT_TRUE(strstr(line, "\"forced\":true") != NULL);
    ASSERT_TRUE(rec_decode(a, line, len, &back, err, sizeof err));
    ASSERT_TRUE(back.forced);
    rec.forced = false;

    t_begin("rec: a commit without intent and behavior is malformed");
    const char *msg_only =
        "{\"type\":\"commit\",\"id\":\"L1\",\"session\":null,\"file\":\"f\","
        "\"op\":\"edit\",\"old_start\":1,\"old_lines\":0,\"new_start\":1,"
        "\"new_lines\":0,\"eof_nl\":true,\"old_text\":[],\"new_text\":[],"
        "\"msg\":\"m\",\"ts\":\"t\",\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, msg_only, strlen(msg_only), &back, err,
                            sizeof err));
    const char *no_behavior =
        "{\"type\":\"commit\",\"id\":\"L1\",\"session\":null,\"file\":\"f\","
        "\"op\":\"edit\",\"old_start\":1,\"old_lines\":0,\"new_start\":1,"
        "\"new_lines\":0,\"eof_nl\":true,\"old_text\":[],\"new_text\":[],"
        "\"intent\":\"i\",\"behavior\":\"\",\"ts\":\"t\",\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, no_behavior, strlen(no_behavior), &back, err,
                            sizeof err));

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
        "\"new_text\":[],\"intent\":\"i\",\"behavior\":\"b\",\"ts\":\"t\","
        "\"prev\":\"p\"}";
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
    RecLog tl;
    char terr[256];
    ASSERT_TRUE(rec_log_parse(a, fsb.data, fsb.len, NULL, NULL, &tl, terr,
                              sizeof terr));
    ASSERT_EQ_I(tl.count, 2);
    ASSERT_TRUE(tl.torn_tail);
    ASSERT_TRUE(tl.torn_bytes > 0);
    ASSERT_TRUE(tl.chain_ok);

    t_begin("rec: a branch record round-trips in its field order");
    Rec br;
    memset(&br, 0, sizeof br);
    br.type = REC_BRANCH;
    br.id = "7c1e9a02d4b8";
    br.name = "parser \"fix\"";
    br.parent = "main";
    br.base = "ab12";
    br.base_chunk = 3;
    br.user = "claude";
    br.ts = "2026-09-27T10:00:00Z";
    br.prev = "ab12";
    size_t blen;
    char *bline = rec_encode(a, &br, &blen);
    ASSERT_TRUE(strncmp(bline,
                        "{\"type\":\"branch\",\"id\":\"7c1e9a02d4b8\","
                        "\"name\":\"parser \\\"fix\\\"\",\"parent\":\"main\","
                        "\"base\":\"ab12\",\"base_chunk\":3,\"user\":"
                        "\"claude\",\"ts\":",
                        strlen("{\"type\":\"branch\",\"id\":\"7c1e9a02d4b8\","
                               "\"name\":\"parser \\\"fix\\\"\",\"parent\":"
                               "\"main\",\"base\":\"ab12\",\"base_chunk\":3,"
                               "\"user\":\"claude\",\"ts\":")) == 0);
    Rec bback;
    ASSERT_TRUE(rec_decode(a, bline, blen, &bback, err, sizeof err));
    ASSERT_TRUE(bback.type == REC_BRANCH);
    ASSERT_EQ_S(bback.id, "7c1e9a02d4b8");
    ASSERT_EQ_S(bback.name, "parser \"fix\"");
    ASSERT_EQ_S(bback.parent, "main");
    ASSERT_EQ_S(bback.base, "ab12");
    ASSERT_EQ_I(bback.base_chunk, 3);
    ASSERT_EQ_S(bback.user, "claude");
    ASSERT_EQ_S(bback.hash, br.hash);

    t_begin("rec: a branch record without its base chunk is refused");
    const char *nobase = "{\"type\":\"branch\",\"id\":\"7c1e9a02d4b8\","
                         "\"name\":\"x\",\"parent\":\"main\",\"base\":\"ab\","
                         "\"ts\":\"t\",\"prev\":\"ab\"}";
    ASSERT_TRUE(!rec_decode(a, nobase, strlen(nobase), &bback, err,
                            sizeof err));

    t_begin("rec: a merge record round-trips with its stopped files");
    Rec mr;
    memset(&mr, 0, sizeof mr);
    mr.type = REC_MERGE;
    mr.branch = "7c1e9a02d4b8";
    mr.name = "feat";
    mr.head = "cafe";
    mr.adopted = 41;
    mr.left = 6;
    const char *sf[] = {"src/a.c", "src/b \"q\".c"};
    const char *sa[] = {"aa", "bb"};
    mr.stopped_file = sf;
    mr.stopped_at = sa;
    mr.stopped_n = 2;
    mr.user = "claude";
    mr.ts = "2026-09-27T10:00:00Z";
    mr.prev = "ab";
    size_t mlen;
    char *mline = rec_encode(a, &mr, &mlen);
    ASSERT_TRUE(strstr(mline, "\"stopped\":[{\"file\":\"src/a.c\",\"at\":"
                              "\"aa\"},{\"file\":") != NULL);
    Rec mback;
    ASSERT_TRUE(rec_decode(a, mline, mlen, &mback, err, sizeof err));
    ASSERT_TRUE(mback.type == REC_MERGE);
    ASSERT_EQ_S(mback.branch, "7c1e9a02d4b8");
    ASSERT_EQ_S(mback.head, "cafe");
    ASSERT_EQ_I(mback.adopted, 41);
    ASSERT_EQ_I(mback.left, 6);
    ASSERT_EQ_I(mback.stopped_n, 2);
    ASSERT_EQ_S(mback.stopped_file[1], "src/b \"q\".c");
    ASSERT_EQ_S(mback.stopped_at[1], "bb");
    ASSERT_EQ_S(mback.hash, mr.hash);
    ASSERT_TRUE(strstr(mline, "\"already\"") == NULL);
    ASSERT_EQ_I(mback.already_n, 0);

    t_begin("rec: a merge record's already list round-trips; a bad one is "
            "refused");
    const char *al[] = {"c0ffee", "decade"};
    mr.already = al;
    mr.already_n = 2;
    mline = rec_encode(a, &mr, &mlen);
    ASSERT_TRUE(strstr(mline, "\"already\":[\"c0ffee\",\"decade\"]") != NULL);
    ASSERT_TRUE(rec_decode(a, mline, mlen, &mback, err, sizeof err));
    ASSERT_EQ_I(mback.already_n, 2);
    ASSERT_EQ_S(mback.already[1], "decade");
    ASSERT_EQ_S(mback.hash, mr.hash);
    const char *bad_al = "{\"type\":\"merge\",\"branch\":\"b\",\"name\":\"n\","
                         "\"head\":\"h\",\"adopted\":0,\"left\":0,"
                         "\"stopped\":[],\"already\":[1],\"ts\":\"t\","
                         "\"prev\":\"p\",\"hash\":\"x\"}";
    ASSERT_TRUE(!rec_decode(a, bad_al, strlen(bad_al), &mback, err,
                            sizeof err));
    mr.already = NULL;
    mr.already_n = 0;

    t_begin("rec: from links survive on commits and sessions, and are absent "
            "otherwise");
    rec.from = "f00d";
    char *fline = rec_encode(a, &rec, &len);
    ASSERT_TRUE(rec_decode(a, fline, len, &back, err, sizeof err));
    ASSERT_EQ_S(back.from, "f00d");
    rec.from = NULL;
    fline = rec_encode(a, &rec, &len);
    ASSERT_TRUE(strstr(fline, "\"from\"") == NULL);
    ASSERT_TRUE(rec_decode(a, fline, len, &back, err, sizeof err));
    ASSERT_TRUE(back.from == NULL);
    Rec ss;
    memset(&ss, 0, sizeof ss);
    ss.type = REC_SESSION_START;
    ss.id = "S3";
    ss.msg = "adopted";
    ss.from = "beef";
    ss.ts = "t";
    ss.prev = "p";
    char *sline = rec_encode(a, &ss, &len);
    ASSERT_TRUE(rec_decode(a, sline, len, &back, err, sizeof err));
    ASSERT_EQ_S(back.from, "beef");
    ss.type = REC_SESSION_END;
    sline = rec_encode(a, &ss, &len);
    ASSERT_TRUE(rec_decode(a, sline, len, &back, err, sizeof err));
    ASSERT_EQ_S(back.from, "beef");

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

    t_begin("rec: a record of a newer type decodes as unknown, in the chain, "
            "its type kept");
    const char *newer = "{\"type\":\"annotate\",\"of\":\"x\",\"intent\":\"i\","
                        "\"ts\":\"t\",\"prev\":\"p\"}";
    Rec nu;
    ASSERT_TRUE(rec_decode(a, newer, strlen(newer), &nu, err, sizeof err));
    ASSERT_TRUE(nu.type == REC_UNKNOWN);
    ASSERT_EQ_S(nu.name, "annotate");
    ASSERT_EQ_S(nu.prev, "p");
    ASSERT_EQ_I((int32_t)strlen(nu.hash), 64);
    const char *no_prev = "{\"type\":\"annotate\",\"ts\":\"t\"}";
    ASSERT_TRUE(!rec_decode(a, no_prev, strlen(no_prev), &nu, err,
                            sizeof err));

    t_begin("rec: a log with newer records parses, counts them, and checks "
            "their chain");
    char *n1 = tline; /* S1 from above, prev zero */
    char *n2 = arena_printf(a, "{\"type\":\"annotate\",\"of\":\"%s\",\"ts\":\"t\","
                               "\"prev\":\"%s\"}",
                            r1.hash, r1.hash);
    Rec later;
    ASSERT_TRUE(rec_decode(a, n2, strlen(n2), &later, err, sizeof err));
    Rec r3;
    memset(&r3, 0, sizeof r3);
    r3.type = REC_SESSION_END;
    r3.id = "S1";
    r3.ts = "t";
    r3.prev = later.hash;
    size_t l3n;
    char *n3 = rec_encode(a, &r3, &l3n);
    char *whole = arena_printf(a, "%s\n%s\n%s\n", n1, n2, n3);
    RecLog nl;
    ASSERT_TRUE(rec_log_parse(a, whole, strlen(whole), NULL, NULL, &nl, err,
                              sizeof err));
    ASSERT_EQ_I(nl.count, 3);
    ASSERT_EQ_I(nl.unknown_n, 1);
    ASSERT_EQ_S(nl.unknown_type, "annotate");
    ASSERT_TRUE(nl.chain_ok);

    t_begin("rec: an amend record round-trips and needs of, intent and "
            "behavior");
    Rec am;
    memset(&am, 0, sizeof am);
    am.type = REC_AMEND;
    am.of = r1.hash;
    am.from = "b1";
    am.user = "ann";
    am.intent = "the right \"why\"";
    am.behavior = "the right what";
    am.forced = true;
    am.ts = "2026-09-28T00:00:00Z";
    am.prev = r1.hash;
    size_t alen;
    char *aline = rec_encode(a, &am, &alen);
    Rec aback;
    ASSERT_TRUE(rec_decode(a, aline, alen, &aback, err, sizeof err));
    ASSERT_TRUE(aback.type == REC_AMEND);
    ASSERT_EQ_S(aback.of, r1.hash);
    ASSERT_EQ_S(aback.from, "b1");
    ASSERT_EQ_S(aback.user, "ann");
    ASSERT_EQ_S(aback.intent, "the right \"why\"");
    ASSERT_EQ_S(aback.behavior, "the right what");
    ASSERT_TRUE(aback.forced);
    ASSERT_EQ_S(aback.hash, am.hash);
    const char *no_of = "{\"type\":\"amend\",\"intent\":\"i\","
                        "\"behavior\":\"b\",\"ts\":\"t\",\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, no_of, strlen(no_of), &aback, err,
                            sizeof err));
    const char *no_bhv = "{\"type\":\"amend\",\"of\":\"x\","
                              "\"intent\":\"i\",\"ts\":\"t\",\"prev\":\"p\"}";
    ASSERT_TRUE(!rec_decode(a, no_bhv, strlen(no_bhv), &aback,
                            err, sizeof err));

    t_begin("rec: amendments apply in log order, the latest text wins and "
            "the earlier ones are kept oldest first");
    RecLog alog;
    memset(&alog, 0, sizeof al);
    Rec v[6];
    memset(v, 0, sizeof v);
    v[0].type = REC_COMMIT;
    v[0].intent = "first why";
    v[0].behavior = "first what";
    v[0].user = "cy";
    v[0].ts = "t0";
    snprintf(v[0].hash, sizeof v[0].hash, "%s", "c0");
    v[1].type = REC_COMMIT;
    v[1].intent = "other why";
    v[1].behavior = "other what";
    snprintf(v[1].hash, sizeof v[1].hash, "%s", "c1");
    v[2] = (Rec){.type = REC_AMEND, .of = "c0", .intent = "second why",
                 .behavior = "second what", .user = "di", .ts = "t2"};
    v[3] = (Rec){.type = REC_AMEND, .of = "nowhere", .intent = "lost",
                 .behavior = "lost"};
    v[4] = (Rec){.type = REC_AMEND, .of = "c0", .intent = "third why",
                 .behavior = "third what", .user = "ed", .ts = "t4",
                 .forced = true};
    v[5] = (Rec){.type = REC_AMEND, .of = "c5", .intent = "too early",
                 .behavior = "too early"};
    alog.v = v;
    alog.count = 6;
    rec_amend_log(a, &alog);
    ASSERT_EQ_I(v[0].amended, 2);
    ASSERT_EQ_S(v[0].intent, "third why");
    ASSERT_EQ_S(v[0].behavior, "third what");
    ASSERT_TRUE(v[0].forced);
    ASSERT_EQ_S(v[0].amend_user, "ed");
    ASSERT_EQ_S(v[0].amend_ts, "t4");
    ASSERT_EQ_I(v[0].earlier_n, 2);
    ASSERT_EQ_S(v[0].earlier_intent[0], "first why");
    ASSERT_EQ_S(v[0].earlier_user[0], "cy");
    ASSERT_EQ_S(v[0].earlier_ts[0], "t0");
    ASSERT_EQ_S(v[0].earlier_behavior[1], "second what");
    ASSERT_EQ_S(v[0].earlier_user[1], "di");
    ASSERT_EQ_S(v[0].earlier_ts[1], "t2");
    ASSERT_EQ_I(v[1].amended, 0);
    ASSERT_EQ_S(v[1].intent, "other why");

    t_begin("rec: an amendment names a commit before it, never one after");
    Rec w[2];
    memset(w, 0, sizeof w);
    w[0] = (Rec){.type = REC_AMEND, .of = "c9", .intent = "early",
                 .behavior = "early"};
    w[1].type = REC_COMMIT;
    w[1].intent = "kept why";
    w[1].behavior = "kept what";
    snprintf(w[1].hash, sizeof w[1].hash, "%s", "c9");
    RecLog wl = {.v = w, .count = 2};
    rec_amend_log(a, &wl);
    ASSERT_EQ_I(w[1].amended, 0);
    ASSERT_EQ_S(w[1].intent, "kept why");

    arena_free(a);
}
