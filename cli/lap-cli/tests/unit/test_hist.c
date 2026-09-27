#include <stdio.h>

#include "hist.h"
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
        "0123456789ab.000001.jsonl", "notes.txt", NULL};
    char path[256];
    for (int32_t i = 0; names[i]; i++) {
        snprintf(path, sizeof path, "%s/%s/%s", T_LAPDIR, LAP_LOG_DIR,
                 names[i]);
        remove(path);
    }
    snprintf(path, sizeof path, "%s/%s", T_LAPDIR, LAP_LOG_NAME);
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

    t_begin("hist: an old file and chunks that differ are refused, both kept");
    clear_chunks();
    put_file("main.000001.jsonl", "aaaa\nXXXX\n");
    put_legacy("aaaa\nbbbb\ncccc\n");
    ASSERT_TRUE(!hist_convert_legacy(a, T_LAPDIR, 100, &converted, err,
                                     sizeof err));
    ASSERT_TRUE(strstr(err, "differ") != NULL);
    ASSERT_TRUE(legacy_exists());
    ASSERT_EQ_S(read_history(a), "aaaa\nXXXX\n");
    clear_chunks();
}

void test_hist(void) {
    Arena *a = arena_new(0);
    test_names();
    test_append_and_read(a);
    test_seal_and_repair(a);
    test_listing(a);
    test_legacy(a);
    arena_free(a);
}
