#include <stdio.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "hist.h"
#include "idx.h"
#include "rec.h"
#include "sha256.h"
#include "test.h"

/* A scratch .lap in the working directory (ctest runs from the build
 * directory, where bin/ may not exist). Only files it names are removed. */
#define T_LAPDIR ".hist_unit_test"

static void clear_chunks(void) {
    static const char *const names[] = {
        "main.000001.jsonl", "main.000002.jsonl", "main.000003.jsonl",
        "main.000004.jsonl", "main.000005.jsonl", "main.000006.jsonl",
        "0123456789ab.000001.jsonl", "0123456789ab.000002.jsonl",
        "ba9876543210.000001.jsonl", "notes.txt", NULL};
    char path[256];
    for (int32_t i = 0; names[i]; i++) {
        snprintf(path, sizeof path, "%s/%s/%s", T_LAPDIR, LAP_LOG_DIR,
                 names[i]);
        remove(path);
    }
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LOG_NAME);
    remove(path);
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LINEAGE_NAME);
    remove(path);
}

static void put_legacy(const char *data) {
    char path[256];
    plat_mkdirs(T_LAPDIR);
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LOG_NAME);
    plat_write_file_atomic(path, data, strlen(data));
}

static bool legacy_exists(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LOG_NAME);
    return plat_is_file(path);
}

static char *read_history(Arena *a) {
    Hist h;
    char err[256], *data = NULL;
    size_t len;
    if (!hist_open(a, T_LAPDIR, "main", &h, err, sizeof err) ||
        !hist_read_all(a, &h, &data, &len))
        return NULL;
    return data;
}

static void put_file(const char *name, const char *data) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s/%s", T_LAPDIR, LAP_LOG_DIR, name);
    plat_write_file_atomic(path, data, strlen(data));
}

static void test_names(void) {
    char lin[HIST_LINEAGE_MAX];
    int32_t n = 0;

    t_begin("hist: chunk names parse into lineage and number");
    ASSERT_TRUE(hist_parse_name("main.000001.jsonl", lin, &n));
    ASSERT_EQ_S(lin, "main");
    ASSERT_EQ_I(n, 1);
    ASSERT_TRUE(hist_parse_name("7c1e9a02d4b8.000042.jsonl", lin, &n));
    ASSERT_EQ_S(lin, "7c1e9a02d4b8");
    ASSERT_EQ_I(n, 42);

    t_begin("hist: anything else is not a chunk");
    ASSERT_TRUE(!hist_parse_name("main.000000.jsonl", lin, &n));
    ASSERT_TRUE(!hist_parse_name("main.00001.jsonl", lin, &n));
    ASSERT_TRUE(!hist_parse_name("main.000001.jsonl.tmp.123", lin, &n));
    ASSERT_TRUE(!hist_parse_name("main.000001.json", lin, &n));
    ASSERT_TRUE(!hist_parse_name("7C1E9A02D4B8.000001.jsonl", lin, &n));
    ASSERT_TRUE(!hist_parse_name("7c1e9a02d4b.000001.jsonl", lin, &n));
    ASSERT_TRUE(!hist_parse_name("feature.000001.jsonl", lin, &n));
    ASSERT_TRUE(!hist_parse_name("log.jsonl", lin, &n));

    t_begin("hist: chunk names are written zero-padded");
    char name[64];
    hist_chunk_name("main", 3, name);
    ASSERT_EQ_S(name, "main.000003.jsonl");
    hist_chunk_name("0123456789ab", 12, name);
    ASSERT_EQ_S(name, "0123456789ab.000012.jsonl");
}

static void test_append_and_read(Arena *a) {
    char err[256];
    Hist h;

    t_begin("hist: a missing log directory is an empty history");
    clear_chunks();
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 0);
    ASSERT_EQ_I(h.size, 0);
    char tail[65];
    ASSERT_TRUE(hist_tail_hash(a, &h, tail));
    ASSERT_EQ_S(tail, LAP_HASH_ZERO);

    t_begin("hist: the first append creates chunk 1");
    h.limit = 20;
    ASSERT_TRUE(hist_append(a, &h, "aaaaaaaaa\n", 10, err, sizeof err));
    ASSERT_EQ_I(h.n, 1);
    ASSERT_EQ_S(h.v[0].name, "main.000001.jsonl");
    ASSERT_EQ_I(h.size, 10);

    t_begin("hist: an append past the limit starts the next chunk");
    ASSERT_TRUE(hist_append(a, &h, "bbbbbbbbb\n", 10, err, sizeof err));
    ASSERT_EQ_I(h.n, 1); /* exactly at the limit still fits */
    ASSERT_TRUE(hist_append(a, &h, "ccccc\n", 6, err, sizeof err));
    ASSERT_EQ_I(h.n, 2);
    ASSERT_EQ_I(h.v[1].start, 20);
    ASSERT_EQ_I(h.v[1].size, 6);
    ASSERT_TRUE(hist_is_sealed(&h, 0));
    ASSERT_TRUE(!hist_is_sealed(&h, 1));

    t_begin("hist: a record larger than the limit is a chunk of its own");
    const char *big = "0123456789012345678901234567890123456789\n";
    ASSERT_TRUE(hist_append(a, &h, big, strlen(big), err, sizeof err));
    ASSERT_EQ_I(h.n, 3);
    ASSERT_EQ_I(h.v[2].size, strlen(big));
    ASSERT_TRUE(hist_append(a, &h, "dd\n", 3, err, sizeof err));
    ASSERT_EQ_I(h.n, 4);

    t_begin("hist: reopening lists the same chunks and offsets");
    Hist h2;
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h2, err, sizeof err));
    ASSERT_EQ_I(h2.n, 4);
    ASSERT_EQ_I(h2.size, h.size);
    ASSERT_EQ_I(h2.v[2].start, 26);
    ASSERT_EQ_I(h2.v[3].start, 26 + (int64_t)strlen(big));

    t_begin("hist: reads span chunk boundaries");
    char *data;
    ASSERT_TRUE(hist_read(a, &h2, 15, 10, &data));
    ASSERT_EQ_S(data, "bbbb\nccccc");
    size_t len;
    ASSERT_TRUE(hist_read_all(a, &h2, &data, &len));
    ASSERT_EQ_I(len, h2.size);
    ASSERT_TRUE(strncmp(data, "aaaaaaaaa\nbbbbbbbbb\nccccc\n0123", 30) == 0);
    ASSERT_TRUE(!hist_read(a, &h2, h2.size - 2, 5, &data));

    t_begin("hist: offsets locate their chunk");
    ASSERT_EQ_I(hist_locate(&h2, 0), 0);
    ASSERT_EQ_I(hist_locate(&h2, 19), 0);
    ASSERT_EQ_I(hist_locate(&h2, 20), 1);
    ASSERT_EQ_I(hist_locate(&h2, h2.size), 3);
    ASSERT_EQ_I(hist_locate(&h2, h2.size + 1), -1);

    t_begin("hist: positions are named by chunk and line");
    ASSERT_TRUE(hist_read_all(a, &h2, &data, &len));
    char where[128];
    hist_where(&h2, data, 10, where, sizeof where);
    ASSERT_EQ_S(where, "main.000001.jsonl line 2");
    hist_where(&h2, data, 20, where, sizeof where);
    ASSERT_EQ_S(where, "main.000002.jsonl line 1");

    t_begin("hist: the tail hash is the last record's");
    char want[65];
    sha256_hex("dd", 2, want);
    ASSERT_TRUE(hist_tail_hash(a, &h2, tail));
    ASSERT_EQ_S(tail, want);
}

static void test_seal_and_repair(Arena *a) {
    char err[256];
    Hist h;
    char tail[65], want[65];

    t_begin("hist: sealing creates the next chunk, empty");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 4);
    ASSERT_TRUE(hist_seal(a, &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 5);
    ASSERT_EQ_I(h.v[4].size, 0);
    ASSERT_EQ_I(h.v[4].start, h.size);

    t_begin("hist: an empty open chunk is not sealed again");
    ASSERT_TRUE(hist_seal(a, &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 5);

    t_begin("hist: the tail hash looks past an empty open chunk");
    sha256_hex("dd", 2, want);
    ASSERT_TRUE(hist_tail_hash(a, &h, tail));
    ASSERT_EQ_S(tail, want);

    t_begin("hist: the next append lands in the sealing chunk");
    ASSERT_TRUE(hist_append(a, &h, "ee\n", 3, err, sizeof err));
    ASSERT_EQ_I(h.n, 5);
    ASSERT_EQ_I(h.v[4].size, 3);

    t_begin("hist: a torn tail in the open chunk is truncated");
    char path[256];
    hist_chunk_path(&h, 4, path, sizeof path);
    ASSERT_TRUE(plat_append_file_sync(path, "{\"torn", 6));
    Hist h2;
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h2, err, sizeof err));
    ASSERT_EQ_I(h2.v[4].size, 9);
    sha256_hex("ee", 2, want);
    ASSERT_TRUE(hist_tail_hash(a, &h2, tail));
    ASSERT_EQ_S(tail, want); /* readers skip it */
    ASSERT_TRUE(hist_repair_torn_tail(a, &h2));
    ASSERT_EQ_I(h2.v[4].size, 3);
    uint64_t fsize = 0;
    ASSERT_TRUE(plat_file_size(path, &fsize));
    ASSERT_EQ_I(fsize, 3);
    ASSERT_EQ_I(h2.size, h.size);

    t_begin("hist: an open chunk holding only a torn line hashes the one before");
    ASSERT_TRUE(hist_seal(a, &h2, err, sizeof err));
    hist_chunk_path(&h2, 5, path, sizeof path);
    ASSERT_TRUE(plat_append_file_sync(path, "{\"torn", 6));
    Hist h3;
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h3, err, sizeof err));
    ASSERT_TRUE(hist_tail_hash(a, &h3, tail));
    ASSERT_EQ_S(tail, want);
}

static void test_listing(Arena *a) {
    char err[256];
    Hist h;

    t_begin("hist: other lineages and stray files are not listed");
    clear_chunks();
    put_file("main.000001.jsonl", "a\n");
    put_file("main.000002.jsonl", "b\n");
    put_file("0123456789ab.000001.jsonl", "x\n");
    put_file("notes.txt", "hello\n");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 2);
    ASSERT_EQ_I(h.size, 4);
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "0123456789ab", &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 1);
    ASSERT_EQ_S(h.v[0].lineage, "0123456789ab");

    t_begin("hist: a missing chunk number is refused, naming it");
    put_file("main.000004.jsonl", "d\n");
    ASSERT_TRUE(!hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(strstr(err, "main.000003.jsonl") != NULL);

    t_begin("hist: parsing a chunked history names chunks in chain breaks");
    clear_chunks();
    Rec r1, r2;
    memset(&r1, 0, sizeof r1);
    r1.type = REC_INIT;
    r1.version = 1;
    r1.ts = "2026-09-27T00:00:00Z";
    r1.prev = LAP_HASH_ZERO;
    size_t l1;
    char *line1 = rec_encode(a, &r1, &l1);
    memset(&r2, 0, sizeof r2);
    r2.type = REC_SESSION_START;
    r2.id = "S1";
    r2.msg = "chunks";
    r2.ts = "2026-09-27T00:00:01Z";
    r2.prev = LAP_HASH_ZERO; /* wrong on purpose: breaks the chain */
    size_t l2;
    char *line2 = rec_encode(a, &r2, &l2);
    put_file("main.000001.jsonl", arena_printf(a, "%s\n", line1));
    put_file("main.000002.jsonl", arena_printf(a, "%s\n", line2));
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    char *data;
    size_t len;
    ASSERT_TRUE(hist_read_all(a, &h, &data, &len));
    RecLog log;
    ASSERT_TRUE(rec_log_parse(a, data, len, NULL, NULL, &log, err,
                              sizeof err));
    ASSERT_TRUE(!log.chain_ok);
    ASSERT_EQ_I(log.chain_break_index, 1);
    ASSERT_EQ_I(log.chain_break_off, l1 + 1);
    ASSERT_TRUE(strstr(log.chain_err, "log line 2") != NULL);
    clear_chunks();

    /* session starts chained from prev; hash receives each one's hash */
    Rec s[4];
    char *sl[4];
    const char *ids[4] = {"S1", "S2", "S3", "S9"};
    for (int32_t i = 0; i < 4; i++) {
        memset(&s[i], 0, sizeof s[i]);
        s[i].type = REC_SESSION_START;
        s[i].id = ids[i];
        s[i].msg = "boundary";
        s[i].ts = "2026-09-27T00:00:01Z";
        /* S9 chains onto S2 like S3: the record appended to a sealed chunk */
        s[i].prev = i == 0 ? LAP_HASH_ZERO : i == 3 ? s[1].hash : s[i - 1].hash;
        size_t n;
        sl[i] = rec_encode(a, &s[i], &n);
    }
#define BREAK_OF(c1, c2, c3)                                                   \
    do {                                                                       \
        clear_chunks();                                                        \
        put_file("main.000001.jsonl", c1);                                     \
        put_file("main.000002.jsonl", c2);                                     \
        if (c3)                                                                \
            put_file("main.000003.jsonl", c3);                                 \
        ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));     \
        ASSERT_TRUE(hist_read_all(a, &h, &data, &len));                        \
        ASSERT_TRUE(rec_log_parse(a, data, len, NULL, NULL, &log, err,         \
                                  sizeof err));                                \
        ASSERT_TRUE(!log.chain_ok);                                            \
        hist_name_break(&h, data, &log);                                       \
    } while (0)
    char *c12 = arena_printf(a, "%s\n%s\n", sl[0], sl[1]);
    char *c3 = arena_printf(a, "%s\n", sl[2]);

    t_begin("hist: a stray chunk continuing another chunk is the one blamed");
    BREAK_OF(c12, c3, c3);
    ASSERT_TRUE(strstr(log.chain_err,
                       "chunk main.000003.jsonl does not belong after "
                       "main.000002.jsonl: it continues main.000001.jsonl") !=
                NULL);

    t_begin("hist: a sealed chunk appended to is the one blamed");
    BREAK_OF(arena_printf(a, "%s%s\n", c12, sl[3]), c3, (char *)NULL);
    ASSERT_TRUE(strstr(log.chain_err,
                       "sealed chunk main.000001.jsonl was modified") != NULL);

    t_begin("hist: a chunk whose first prev leads nowhere names both chunks");
    BREAK_OF(arena_printf(a, "%s\n", sl[0]), c3, (char *)NULL);
    ASSERT_TRUE(strstr(log.chain_err,
                       "chunk main.000002.jsonl does not continue sealed "
                       "chunk main.000001.jsonl") != NULL);

    t_begin("hist_scan: counts records across chunks and follows the chain");
    HistScan sc;
    clear_chunks();
    put_file("main.000001.jsonl", c12);
    put_file("main.000002.jsonl", c3);
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_scan(a, &h, &sc, err, sizeof err));
    ASSERT_EQ_I(sc.records, 3);
    ASSERT_TRUE(sc.chain_ok);
    ASSERT_EQ_S(sc.last_hash, s[2].hash);
    ASSERT_EQ_I((int32_t)sc.torn_bytes, 0);

    t_begin("hist_scan: a broken chain, a torn tail and a newer type are "
            "reported");
    clear_chunks();
    put_file("main.000001.jsonl", arena_printf(a, "%s\n", sl[0]));
    put_file("main.000002.jsonl",
             arena_printf(a, "%s\n{\"type\":\"annotate\",\"ts\":\"t\","
                             "\"prev\":\"%s\"}\n{\"type\":\"comm",
                          sl[2], s[2].hash));
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_scan(a, &h, &sc, err, sizeof err));
    ASSERT_EQ_I(sc.records, 3);
    ASSERT_TRUE(!sc.chain_ok);
    ASSERT_EQ_I((int32_t)sc.torn_bytes, 13); /* {"type":"comm */
    ASSERT_EQ_I(sc.unknown_n, 1);
    ASSERT_EQ_S(sc.unknown_type, "annotate");

    t_begin("hist: a break inside a sealed chunk blames that chunk");
    BREAK_OF(arena_printf(a, "%s\n%s\n", sl[0], sl[2]), c3, (char *)NULL);
    ASSERT_TRUE(strstr(log.chain_err,
                       "sealed chunk main.000001.jsonl was modified") != NULL);
#undef BREAK_OF
    clear_chunks();
}

/* A conversion publishing its chunk and removing the old file, as it
 * would between hist_open's two looks. */
static void publish_between(void) {
    put_file("main.000001.jsonl", "pub1\n");
    char path[256];
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LOG_NAME);
    remove(path);
}

static void test_legacy(Arena *a) {
    char err[256];
    Hist h;
    bool converted = true;

    t_begin("hist: a lone log.jsonl is read as a legacy history");
    clear_chunks();
    put_legacy("aaaa\nbbbb\ncccc\n");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(h.legacy);
    ASSERT_EQ_I(h.n, 1);
    ASSERT_EQ_I(h.size, 15);
    char *data;
    ASSERT_TRUE(hist_read(a, &h, 5, 4, &data));
    ASSERT_EQ_S(data, "bbbb");

    t_begin("hist: an old log beside chunks is read, flagged as differing "
            "only when the chunks are neither all of it nor a start of it");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\n");
    put_legacy("aaaa\nbbbb\n"); /* an interrupted conversion's start */
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(h.legacy && !h.differ);
    put_legacy("xxxx\nbbbb\n"); /* chunks of another history */
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(h.legacy && h.differ);
    put_legacy("aaaa\n"); /* the chunks hold all of it */
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(!h.legacy && !h.differ);

    t_begin("hist: a conversion publishing between the reader's two looks "
            "is read from its chunks, never as no history");
    clear_chunks();
    put_legacy("pub1\n");
    hist_open_between = publish_between;
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    hist_open_between = NULL;
    ASSERT_TRUE(!h.legacy);
    ASSERT_EQ_I(h.n, 1);
    ASSERT_EQ_S(read_history(a), "pub1\n");
    clear_chunks();
    put_legacy("aaaa\nbbbb\ncccc\n");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));

    t_begin("hist: conversion splits at record boundaries and removes the "
            "old file");
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 10, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(converted);
    ASSERT_TRUE(!legacy_exists());
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(!h.legacy);
    ASSERT_EQ_I(h.n, 2);
    ASSERT_EQ_I(h.v[0].size, 10);
    ASSERT_EQ_I(h.v[1].size, 5);
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");

    t_begin("hist: without an old file, conversion does nothing");
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 10, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(!converted);
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");

    t_begin("hist: conversion drops a torn final line");
    clear_chunks();
    put_legacy("aaaa\nbb");
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 100, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(converted);
    ASSERT_EQ_S(read_history(a), "aaaa\n");

    t_begin("hist: an old file with no complete record becomes one empty "
            "chunk, never no history");
    const char *empties[] = {"", "{\"type\":\"init", NULL};
    for (int32_t i = 0; empties[i]; i++) {
        clear_chunks();
        put_legacy(empties[i]);
        ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 100, &converted, err,
                                        sizeof err));
        ASSERT_TRUE(converted);
        ASSERT_TRUE(!legacy_exists());
        Hist e;
        ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &e, err, sizeof err));
        ASSERT_EQ_I(e.n, 1);
        ASSERT_EQ_I((int32_t)e.size, 0);
    }

    t_begin("hist: chunks that are a prefix of the old file are redone");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\n");
    put_legacy("aaaa\nbbbb\ncccc\n");
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 100, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(converted);
    ASSERT_TRUE(!legacy_exists());
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");

    t_begin("hist: an old file that is a prefix of the chunks is a leftover");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\nbbbb\n");
    put_file("main.000002.jsonl", "cccc\n");
    put_legacy("aaaa\nbbbb\n");
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 100, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(!converted);
    ASSERT_TRUE(!legacy_exists());
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");

    /* three chained records, and a fourth appended after them */
    Rec q[4];
    char *ql[4];
    for (int32_t i = 0; i < 4; i++) {
        memset(&q[i], 0, sizeof q[i]);
        q[i].type = REC_SESSION_START;
        q[i].id = arena_printf(a, "S%d", i + 1);
        q[i].msg = "limits";
        q[i].ts = "2026-09-27T00:00:01Z";
        q[i].prev = i ? q[i - 1].hash : LAP_HASH_ZERO;
        size_t n;
        ql[i] = rec_encode(a, &q[i], &n);
    }
    const char *old3 = arena_printf(a, "%s\n%s\n%s\n", ql[0], ql[1], ql[2]);

    t_begin("hist: chunks holding the old file and records appended since "
            "are the finished conversion");
    clear_chunks();
    put_file("main.000001.jsonl", old3);
    put_file("main.000002.jsonl", arena_printf(a, "%s\n", ql[3]));
    put_legacy(old3);
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 100000, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(!converted);
    ASSERT_TRUE(!legacy_exists());
    ASSERT_EQ_S(read_history(a), arena_printf(a, "%s%s\n", old3, ql[3]));

    t_begin("hist: leftovers of a run under another chunk limit past the "
            "old file are converted again, keeping the chain whole");
    clear_chunks();
    put_file("main.000001.jsonl", old3);
    put_file("main.000002.jsonl", arena_printf(a, "%s\n", ql[1]));
    put_file("main.000003.jsonl", arena_printf(a, "%s\n", ql[2]));
    put_legacy(old3);
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 100000, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(converted);
    ASSERT_TRUE(!legacy_exists());
    ASSERT_EQ_S(read_history(a), old3);

    t_begin("hist: an old file and chunks that differ are refused, both kept");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\nXXXX\n");
    put_legacy("aaaa\nbbbb\ncccc\n");
    ASSERT_TRUE(!hist_convert_legacy(a, T_LAPDIR, 100, &converted, err,
                                     sizeof err));
    ASSERT_TRUE(strstr(err, "differ") != NULL);
    ASSERT_TRUE(legacy_exists());
    /* readers take the old file: the chunks do not hold all of it */
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");

    t_begin("hist: readers finding both shapes read the old file unless the "
            "chunks hold all of it");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\n"); /* an interrupted conversion */
    put_legacy("aaaa\nbbbb\n");
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\n");
    put_file("main.000002.jsonl", "bbbb\ncccc\n"); /* all of it, and more */
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");

    t_begin("hist: conversion publishes log/ whole and leaves no working "
            "folder behind");
    clear_chunks();
    put_legacy("aaaa\nbbbb\ncccc\n");
    ASSERT_TRUE(hist_convert_legacy(a, T_LAPDIR, 6, &converted, err,
                                    sizeof err));
    ASSERT_TRUE(converted && !legacy_exists());
    ASSERT_TRUE(!plat_is_dir(T_LAPDIR "/log.converting"));
    ASSERT_TRUE(!plat_is_dir(T_LAPDIR "/log.replaced"));
    ASSERT_EQ_S(read_history(a), "aaaa\nbbbb\ncccc\n");
    clear_chunks();
}

static void test_branch_history(Arena *a) {
    char err[256];
    char lin[HIST_LINEAGE_MAX];
    Hist h;

    t_begin("hist: a folder without a lineage file writes main");
    clear_chunks();
    ASSERT_TRUE(hist_folder_lineage(a, T_LAPDIR, lin, err, sizeof err));
    ASSERT_EQ_S(lin, "main");

    t_begin("hist: the lineage file names a branch id, and nothing else");
    ASSERT_TRUE(hist_write_lineage(T_LAPDIR, "0123456789ab"));
    ASSERT_TRUE(hist_folder_lineage(a, T_LAPDIR, lin, err, sizeof err));
    ASSERT_EQ_S(lin, "0123456789ab");
    char path[256];
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LINEAGE_NAME);
    plat_write_file_atomic(path, "main\n", 5);
    ASSERT_TRUE(!hist_folder_lineage(a, T_LAPDIR, lin, err, sizeof err));
    plat_write_file_atomic(path, "0123456789AB", 12);
    ASSERT_TRUE(!hist_folder_lineage(a, T_LAPDIR, lin, err, sizeof err));

    t_begin("hist: a lineage whose recorded parent is this folder itself is "
            "ignored; one whose parent is elsewhere stands");
    ASSERT_TRUE(hist_write_lineage(T_LAPDIR, "0123456789ab"));
    char ppath[256];
    snprintf(ppath, sizeof ppath, "%s/%s", T_LAPDIR, LAP_PARENT_NAME);
    plat_write_file_atomic(ppath, "..\n", 3); /* another folder */
    ASSERT_TRUE(hist_folder_lineage(a, T_LAPDIR, lin, err, sizeof err));
    ASSERT_EQ_S(lin, "0123456789ab");
    /* this test's lapdir is not named .lap, so it is its own root */
    plat_write_file_atomic(ppath, T_LAPDIR "\n", strlen(T_LAPDIR) + 1);
    ASSERT_TRUE(hist_folder_lineage(a, T_LAPDIR, lin, err, sizeof err));
    ASSERT_EQ_S(lin, "main");
    remove(ppath);

    t_begin("hist: a branch's history is its parent's chunks to the base, "
            "then its own");
    clear_chunks();
    put_file("main.000001.jsonl", "p1\np2\n");
    put_file("main.000002.jsonl", "p3\n");
    put_file("main.000003.jsonl", "after-the-base\n");
    Rec br;
    memset(&br, 0, sizeof br);
    br.type = REC_BRANCH;
    br.id = "0123456789ab";
    br.name = "feat";
    br.parent = "main";
    br.base = "ab";
    br.base_chunk = 2;
    br.ts = "2026-09-27T00:00:00Z";
    br.prev = "ab";
    size_t blen;
    char *bline = rec_encode(a, &br, &blen);
    put_file("0123456789ab.000001.jsonl", arena_printf(a, "%s\nb1\n", bline));
    ASSERT_TRUE(hist_write_lineage(T_LAPDIR, "0123456789ab"));
    ASSERT_TRUE(hist_open_folder(a, T_LAPDIR, &h, err, sizeof err));
    ASSERT_EQ_S(h.lineage, "0123456789ab");
    ASSERT_EQ_S(h.parent, "main");
    ASSERT_EQ_S(h.name, "feat");
    ASSERT_EQ_I(h.base_chunk, 2);
    ASSERT_EQ_I(h.n, 3);
    ASSERT_EQ_S(h.v[2].name, "0123456789ab.000001.jsonl");
    ASSERT_EQ_I(h.v[2].start, 9);
    ASSERT_EQ_S(hist_label(&h, 0), "main");
    ASSERT_EQ_S(hist_label(&h, 1), "main");
    ASSERT_EQ_S(hist_label(&h, 2), "feat");
    char *data;
    size_t len;
    ASSERT_TRUE(hist_read_all(a, &h, &data, &len));
    ASSERT_EQ_S(data, arena_printf(a, "p1\np2\np3\n%s\nb1\n", bline));

    t_begin("hist: a branch appends to its own chunk, never its parent's");
    h.limit = 1000;
    ASSERT_TRUE(hist_append(a, &h, "b2\n", 3, err, sizeof err));
    ASSERT_EQ_I(h.n, 3);
    ASSERT_EQ_S(read_history(a), "p1\np2\np3\nafter-the-base\n");

    t_begin("hist: a branch whose base chunk is missing is refused");
    char p2[256];
    snprintf(p2, sizeof p2, "%s/%s/main.000002.jsonl", T_LAPDIR, LAP_LOG_DIR);
    remove(p2);
    snprintf(p2, sizeof p2, "%s/%s/main.000003.jsonl", T_LAPDIR, LAP_LOG_DIR);
    remove(p2);
    ASSERT_TRUE(!hist_open_folder(a, T_LAPDIR, &h, err, sizeof err));
    ASSERT_TRUE(strstr(err, "main.000002.jsonl") != NULL);

    t_begin("hist: a branch chunk that does not open with its branch record "
            "is refused");
    put_file("main.000002.jsonl", "p3\n");
    put_file("0123456789ab.000001.jsonl", "b1\n");
    ASSERT_TRUE(!hist_open_folder(a, T_LAPDIR, &h, err, sizeof err));
    clear_chunks();
}

/* A branch record line for id, started from parent after its chunk bc. */
static char *branch_line(Arena *a, const char *id, const char *name,
                         const char *parent, int32_t bc) {
    Rec br;
    memset(&br, 0, sizeof br);
    br.type = REC_BRANCH;
    br.id = id;
    br.name = name;
    br.parent = parent;
    br.base = "ab";
    br.base_chunk = bc;
    br.ts = "2026-09-27T00:00:00Z";
    br.prev = "ab";
    size_t len;
    return rec_encode(a, &br, &len);
}

static void test_nested_history(Arena *a) {
    char err[256];
    Hist h;
    const char *one = "0123456789ab", *two = "ba9876543210";

    t_begin("hist: a branch of a branch reads main to the first branch's "
            "base, that branch to its own base, then its own, all named");
    clear_chunks();
    put_file("main.000001.jsonl", "p1\n");
    put_file("main.000002.jsonl", "p-after\n");
    char *l1 = branch_line(a, one, "one", "main", 1);
    char *l2 = branch_line(a, two, "two", one, 1);
    put_file("0123456789ab.000001.jsonl", arena_printf(a, "%s\nb1\n", l1));
    put_file("0123456789ab.000002.jsonl", "b1-after\n");
    put_file("ba9876543210.000001.jsonl", arena_printf(a, "%s\nc1\n", l2));
    ASSERT_TRUE(hist_open_lineage(a, T_LAPDIR, two, &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 3);
    ASSERT_EQ_S(h.v[0].name, "main.000001.jsonl");
    ASSERT_EQ_S(h.v[1].name, "0123456789ab.000001.jsonl");
    ASSERT_EQ_S(h.v[2].name, "ba9876543210.000001.jsonl");
    ASSERT_EQ_S(h.parent, one);
    ASSERT_EQ_S(hist_label(&h, 0), "main");
    ASSERT_EQ_S(hist_label(&h, 1), "one");
    ASSERT_EQ_S(hist_label(&h, 2), "two");
    char *data;
    size_t len;
    ASSERT_TRUE(hist_read_all(a, &h, &data, &len));
    ASSERT_EQ_S(data, arena_printf(a, "p1\n%s\nb1\n%s\nc1\n", l1, l2));

    t_begin("hist: a view is cut after one of its own chunks");
    ASSERT_TRUE(hist_open_view(a, T_LAPDIR, one, 1, &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 2);
    ASSERT_TRUE(hist_open_view(a, T_LAPDIR, one, 0, &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 3);
    ASSERT_TRUE(hist_open_view(a, T_LAPDIR, "main", 1, &h, err, sizeof err));
    ASSERT_EQ_I(h.n, 1);
    ASSERT_EQ_I((int32_t)h.size, 3);
    ASSERT_TRUE(!hist_open_view(a, T_LAPDIR, one, 3, &h, err, sizeof err));
    ASSERT_TRUE(strstr(err, "0123456789ab.000003.jsonl") != NULL);

    t_begin("hist: a missing base in the middle of the chain is refused, "
            "naming the branch");
    put_file("0123456789ab.000001.jsonl", "");
    ASSERT_TRUE(!hist_open_lineage(a, T_LAPDIR, two, &h, err, sizeof err));
    ASSERT_TRUE(strstr(err, "two") != NULL);

    t_begin("hist: branch records that lead back to themselves are a loop");
    put_file("ba9876543210.000001.jsonl",
             arena_printf(a, "%s\n", branch_line(a, two, "two", two, 1)));
    ASSERT_TRUE(!hist_open_lineage(a, T_LAPDIR, two, &h, err, sizeof err));
    ASSERT_TRUE(strstr(err, "loop") != NULL);
    clear_chunks();
}

static void test_check(Arena *a) {
    char err[256];
    Hist h;
    Rec r0, r1, r2;
    memset(&r0, 0, sizeof r0);
    r0.type = REC_INIT;
    r0.version = 1;
    r0.ts = "t0";
    r0.prev = LAP_HASH_ZERO;
    size_t n0, n1, n2;
    char *l0 = rec_encode(a, &r0, &n0);
    memset(&r1, 0, sizeof r1);
    r1.type = REC_SESSION_START;
    r1.id = "S1";
    r1.msg = "work";
    r1.ts = "t1";
    r1.prev = r0.hash;
    char *l1 = rec_encode(a, &r1, &n1);
    memset(&r2, 0, sizeof r2);
    r2.type = REC_SESSION_END;
    r2.id = "S1";
    r2.ts = "t2";
    r2.prev = r1.hash;
    char *l2 = rec_encode(a, &r2, &n2);

    t_begin("hist_check: whole chunks that chain pass, a torn open chunk "
            "included");
    clear_chunks();
    put_file("main.000001.jsonl", arena_printf(a, "%s\n%s\n", l0, l1));
    put_file("main.000002.jsonl", arena_printf(a, "%s\n{\"torn", l2));
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_check(a, &h, true, err, sizeof err));

    t_begin("hist_check: a sealed chunk ending in a torn line is refused, "
            "named, even behind an empty open chunk");
    put_file("main.000001.jsonl", arena_printf(a, "%s\n%s\n{\"torn", l0, l1));
    put_file("main.000002.jsonl", "");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(!hist_check(a, &h, false, err, sizeof err));
    ASSERT_TRUE(strstr(err, "main.000001.jsonl ends in a torn line") != NULL);

    t_begin("hist_check: with chain, a chunk not continuing the one before "
            "is refused, named; without, it passes");
    put_file("main.000001.jsonl", arena_printf(a, "%s\n", l0));
    put_file("main.000002.jsonl", arena_printf(a, "%s\n", l2)); /* skips l1 */
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_check(a, &h, false, err, sizeof err));
    ASSERT_TRUE(!hist_check(a, &h, true, err, sizeof err));
    ASSERT_TRUE(strstr(err, "main.000002.jsonl does not continue "
                            "main.000001.jsonl") != NULL);

    t_begin("hist_check: with chain, the open chunk's own chain is checked, "
            "naming the line; a sealed chunk's is left to verify");
    clear_chunks();
    put_file("main.000001.jsonl", arena_printf(a, "%s\n%s\n%s\n{\"torn",
                                               l0, l1, l2));
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_check(a, &h, true, err, sizeof err));
    /* a line repeated, as a union-style git resolution leaves it */
    put_file("main.000001.jsonl", arena_printf(a, "%s\n%s\n%s\n%s\n", l0, l1,
                                               l2, l2));
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_check(a, &h, false, err, sizeof err));
    ASSERT_TRUE(!hist_check(a, &h, true, err, sizeof err));
    ASSERT_TRUE(strstr(err, "hash chain broken at main.000001.jsonl line 4") !=
                NULL);
    put_file("main.000001.jsonl", arena_printf(a, "%s\n%s\n", l0, l0));
    put_file("main.000002.jsonl", "");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_check(a, &h, true, err, sizeof err)); /* sealed */

    t_begin("hist_check: an open chunk holding only a torn line is an empty "
            "chunk with a torn tail, not a broken chain");
    clear_chunks();
    put_file("main.000001.jsonl", arena_printf(a, "%s\n%s\n", l0, l1));
    put_file("main.000002.jsonl", "{\"type\":\"commit\",\"id\":\"L9");
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(hist_check(a, &h, true, err, sizeof err));

    t_begin("hist_check: a sealed chunk holding only a torn line is still "
            "refused");
    put_file("main.000002.jsonl", "{\"type\":\"commit\",\"id\":\"L9");
    put_file("main.000003.jsonl", arena_printf(a, "%s\n", l2));
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &h, err, sizeof err));
    ASSERT_TRUE(!hist_check(a, &h, true, err, sizeof err));
    ASSERT_TRUE(strstr(err, "main.000002.jsonl ends in a torn line") != NULL);
    clear_chunks();
}

static void test_tmp_files(Arena *a) {
    char dir[256], blocker[256], left[256];
    snprintf(dir, sizeof dir, "%s/%s", T_LAPDIR, LAP_LOG_DIR);
    plat_mkdirs(dir);

#ifndef _WIN32
    t_begin("hist: a chunk is written with its temp file outside log/");
    clear_chunks();
    /* a folder where the temp file would be if it were made in log/: the
     * write can only succeed by putting it elsewhere */
    snprintf(blocker, sizeof blocker, "%s/main.000001.jsonl.tmp.%lu", dir,
             (unsigned long)getpid());
    plat_mkdirs(blocker);
    ASSERT_TRUE(hist_write_chunk(dir, "main.000001.jsonl", "aaaa\n", 5));
    ASSERT_EQ_S(read_history(a), "aaaa\n");
    plat_rmdir(blocker);
#endif

    t_begin("hist: leftover temp files in log/ are removed, chunks kept, "
            "and never read as chunks");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\n");
    snprintf(left, sizeof left, "%s/main.000002.jsonl.tmp.4242", dir);
    plat_write_file_atomic(left, "bbbb\n", 5);
    ASSERT_EQ_S(read_history(a), "aaaa\n");
    hist_clear_tmp(a, T_LAPDIR);
    ASSERT_TRUE(!plat_is_file(left));
    ASSERT_EQ_S(read_history(a), "aaaa\n");

    t_begin("hist: leftovers outside log/ go too: a chunk temp in .lap/, a "
            "shadow temp at any depth, a finished conversion's folders");
    char ctmp[256], stmp[256], skeep[256], conv[256], repl[256], rfile[256];
    snprintf(ctmp, sizeof ctmp, "%s/main.000002.jsonl.tmp.4243", T_LAPDIR);
    snprintf(stmp, sizeof stmp, "%s/shadow/d/f.txt.tmp.4244", T_LAPDIR);
    snprintf(skeep, sizeof skeep, "%s/shadow/d/f.txt", T_LAPDIR);
    snprintf(conv, sizeof conv, "%s/log.converting", T_LAPDIR);
    snprintf(repl, sizeof repl, "%s/log.replaced", T_LAPDIR);
    snprintf(rfile, sizeof rfile, "%s/log.replaced/main.000001.jsonl",
             T_LAPDIR);
    plat_mkdirs(T_LAPDIR "/shadow/d");
    plat_mkdirs(conv);
    plat_mkdirs(repl);
    plat_write_file_atomic(ctmp, "x", 1);
    plat_write_file_atomic(stmp, "x", 1);
    plat_write_file_atomic(skeep, "kept", 4);
    plat_write_file_atomic(rfile, "old\n", 4);
    hist_clear_tmp(a, T_LAPDIR);
    ASSERT_TRUE(!plat_is_file(ctmp));
    ASSERT_TRUE(!plat_is_file(stmp));
    ASSERT_TRUE(plat_is_file(skeep));
    ASSERT_TRUE(!plat_is_dir(conv) && !plat_is_dir(repl));
    ASSERT_EQ_S(read_history(a), "aaaa\n");
    remove(skeep);
    plat_rmdir(T_LAPDIR "/shadow/d");
    plat_rmdir(T_LAPDIR "/shadow");

    t_begin("hist: past the last chunk number nothing is created, and the "
            "history says so");
    Hist full;
    char err[256];
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &full, err, sizeof err));
    full.v[full.n - 1].n = HIST_MAX_CHUNK; /* as if it were the last */
    ASSERT_TRUE(!hist_seal(a, &full, err, sizeof err));
    ASSERT_TRUE(strstr(err, "last chunk number") != NULL);
    ASSERT_EQ_I(full.n, 1);
    ASSERT_EQ_S(read_history(a), "aaaa\n");
    clear_chunks();
}

/* A commit of rel, chained after prev, for the index check. */
static char *commit_line(Arena *a, const char *id, const char *rel,
                         const char *op, const char *prev, Rec *out) {
    memset(out, 0, sizeof *out);
    out->type = REC_COMMIT;
    out->id = id;
    out->file = rel;
    out->op = op;
    out->intent = "check the index";
    out->behavior = "a commit for the index check";
    out->ts = "t1";
    out->eof_nl = true;
    out->prev = prev;
    size_t n;
    return rec_encode(a, out, &n);
}

static void test_index_match(Arena *a) {
    char why[256], err[256];
    Rec r0, c1, c2, c3;
    memset(&r0, 0, sizeof r0);
    r0.type = REC_INIT;
    r0.version = 1;
    r0.ts = "t0";
    r0.prev = LAP_HASH_ZERO;
    size_t n0;
    char *l0 = rec_encode(a, &r0, &n0);
    char *l1 = commit_line(a, "L1", "a.txt", "create", r0.hash, &c1);
    char *l2 = commit_line(a, "L2", "b.txt", "create", c1.hash, &c2);
    char *l3 = commit_line(a, "L3", "a.txt", "edit", c2.hash, &c3);
    clear_chunks();
    put_file("main.000001.jsonl",
             arena_printf(a, "%s\n%s\n%s\n%s\n", l0, l1, l2, l3));
    Repo r;
    memset(&r, 0, sizeof r);
    ASSERT_TRUE(hist_open(a, T_LAPDIR, "main", &r.hist, err, sizeof err));
    /* the index a rebuild would make */
    IdxEntry v[4];
    memset(v, 0, sizeof v);
    uint64_t off = 0;
    const char *lines[] = {l0, l1, l2, l3};
    for (int32_t i = 0; i < 4; i++) {
        v[i].off = off;
        v[i].len = (uint32_t)strlen(lines[i]);
        v[i].kind = i == 0 ? IDX_INIT : IDX_COMMIT;
        v[i].file_id = UINT32_MAX;
        v[i].prev_same_file = -1;
        off += v[i].len + 1;
    }
    v[1].file_id = 0, v[1].op = IDX_OP_CREATE;
    v[2].file_id = 1, v[2].op = IDX_OP_CREATE;
    v[3].file_id = 0, v[3].op = IDX_OP_EDIT, v[3].prev_same_file = 1;
    char *paths[] = {"a.txt", "b.txt"};
    FileHead heads[2];
    memset(heads, 0, sizeof heads);
    heads[0].head = 3;
    heads[1].head = 2;
    Idx ix;
    memset(&ix, 0, sizeof ix);
    ix.h.count = 4;
    ix.v = v;
    ix.paths = paths;
    ix.heads = heads;
    ix.npaths = 2;

    t_begin("idx_matches_log: an index as a rebuild makes it matches");
    ASSERT_TRUE(idx_matches_log(a, &r, &ix, why, sizeof why));

    t_begin("idx_matches_log: a wrong head, chain, file, place or count is "
            "named");
    heads[0].head = -1;
    ASSERT_TRUE(!idx_matches_log(a, &r, &ix, why, sizeof why));
    ASSERT_TRUE(strstr(why, "index: the last commit it gives a.txt") !=
                NULL);
    heads[0].head = 3;
    v[3].prev_same_file = -1;
    ASSERT_TRUE(!idx_matches_log(a, &r, &ix, why, sizeof why));
    ASSERT_TRUE(strstr(why, "a.txt's chain") != NULL);
    v[3].prev_same_file = 1;
    v[2].file_id = 0;
    ASSERT_TRUE(!idx_matches_log(a, &r, &ix, why, sizeof why));
    ASSERT_TRUE(strstr(why, "L2's commit to b.txt") != NULL);
    v[2].file_id = 1;
    v[2].off += 1;
    ASSERT_TRUE(!idx_matches_log(a, &r, &ix, why, sizeof why));
    v[2].off -= 1;
    ix.h.count = 3;
    ASSERT_TRUE(!idx_matches_log(a, &r, &ix, why, sizeof why));
    ASSERT_TRUE(strstr(why, "L3 has no entry") != NULL);
    ix.h.count = 4;
    ASSERT_TRUE(idx_matches_log(a, &r, &ix, why, sizeof why));
    clear_chunks();
}

static void test_lineages(Arena *a) {
    t_begin("hist_lineages: branches by their first chunk; "
            "hist_lineages_any: every branch with a chunk here, once");
    clear_chunks();
    put_file("main.000001.jsonl", "m\n");
    put_file("0123456789ab.000002.jsonl", "x\n"); /* its chunk 1 is gone */
    put_file("ba9876543210.000001.jsonl", "y\n");
    put_file("notes.txt", "n\n");
    const char **ids;
    ASSERT_EQ_I(hist_lineages(a, T_LAPDIR, &ids), 1);
    ASSERT_EQ_S(ids[0], "ba9876543210");
    ASSERT_EQ_I(hist_lineages_any(a, T_LAPDIR, &ids), 2);
    ASSERT_EQ_S(ids[0], "0123456789ab");
    ASSERT_EQ_S(ids[1], "ba9876543210");
    put_file("0123456789ab.000001.jsonl", "w\n");
    ASSERT_EQ_I(hist_lineages_any(a, T_LAPDIR, &ids), 2); /* not twice */
    clear_chunks();
}

/* A branch record of id, started from parent at its chunk base_chunk. */
static char *lost_branch_line(Arena *a, const char *id, const char *parent,
                         int32_t base_chunk) {
    Rec br;
    memset(&br, 0, sizeof br);
    br.type = REC_BRANCH;
    br.id = id;
    br.name = id;
    br.parent = parent;
    br.base = LAP_HASH_ZERO;
    br.base_chunk = base_chunk;
    br.ts = "t0";
    br.prev = LAP_HASH_ZERO;
    size_t n;
    return arena_printf(a, "%s\n", rec_encode(a, &br, &n));
}

static void test_lost_lineage(Arena *a) {
    t_begin("hist_lost_lineage: a folder with a branch's chunks and its "
            "parent's up to the base only, but no lineage file, is that "
            "branch's; the parent's own folder, past the base, is not");
    clear_chunks();
    put_file("main.000001.jsonl", "m\n");
    put_file("0123456789ab.000001.jsonl",
             lost_branch_line(a, "0123456789ab", "main", 1));
    const char *lost = hist_lost_lineage(a, T_LAPDIR);
    ASSERT_TRUE(lost && strcmp(lost, "0123456789ab") == 0);
    put_file("main.000002.jsonl", ""); /* the parent sealed and went on */
    ASSERT_TRUE(hist_lost_lineage(a, T_LAPDIR) == NULL);

    t_begin("hist_lost_lineage: of a branch of a branch, the deepest; "
            "nothing when the lineage file is there");
    clear_chunks();
    put_file("main.000001.jsonl", "m\n");
    put_file("0123456789ab.000001.jsonl",
             lost_branch_line(a, "0123456789ab", "main", 1));
    put_file("ba9876543210.000001.jsonl",
             lost_branch_line(a, "ba9876543210", "0123456789ab", 1));
    lost = hist_lost_lineage(a, T_LAPDIR);
    ASSERT_TRUE(lost && strcmp(lost, "ba9876543210") == 0);
    char lpath[256];
    snprintf(lpath, sizeof lpath, "%s/%s", T_LAPDIR, LAP_LINEAGE_NAME);
    plat_write_file_atomic(lpath, "ba9876543210\n", 13);
    ASSERT_TRUE(hist_lost_lineage(a, T_LAPDIR) == NULL);
    clear_chunks();
}

static void test_first_record_unreadable(Arena *a) {
#ifndef _WIN32
    if (getuid() == 0)
        return; /* root reads through any permission */
    t_begin("hist_first_record: a chunk it cannot reach is named as "
            "unreadable, not as missing");
    clear_chunks();
    put_file("0123456789ab.000001.jsonl",
             lost_branch_line(a, "0123456789ab", "main", 1));
    char logdir[256], err[512];
    snprintf(logdir, sizeof logdir, "%s/%s", T_LAPDIR, LAP_LOG_DIR);
    Rec br;
    ASSERT_TRUE(hist_first_record(a, T_LAPDIR, "0123456789ab", &br, err,
                                  sizeof err));
    ASSERT_TRUE(chmod(logdir, 0) == 0);
    ASSERT_TRUE(!hist_first_record(a, T_LAPDIR, "0123456789ab", &br, err,
                                   sizeof err));
    chmod(logdir, 0755);
    ASSERT_TRUE(strstr(err, "cannot be reached (no permission") != NULL);
    ASSERT_TRUE(strstr(err, "missing or empty") == NULL);
    clear_chunks();
#else
    (void)a;
#endif
}

void test_hist(void) {
    Arena *a = arena_new(0);
    test_names();
    test_append_and_read(a);
    test_seal_and_repair(a);
    test_listing(a);
    test_legacy(a);
    test_branch_history(a);
    test_nested_history(a);
    test_check(a);
    test_index_match(a);
    test_lineages(a);
    test_lost_lineage(a);
    test_first_record_unreadable(a);
    test_tmp_files(a);
    arena_free(a);
}
