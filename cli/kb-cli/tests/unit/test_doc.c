/* §1.1 identifiers and §1.6's log records.
 *
 * Rewritten after the working tree was lost.
 *
 * The encoding is the part worth testing hardest. `sources.jsonl` and
 * `documents.jsonl` are append-only and are the truth (§1.6), and the store
 * frames records by newline — so a record that can contain a raw newline can
 * split itself into two lines, and the second one is garbage that the next
 * read either rejects or, worse, half-believes. Everything a caller supplies
 * is attacker-shaped here: a title comes from a web page and `meta` is
 * free-form by §1.2. */
#include <string.h>

#include "test.h"
#include "../../src/doc.h"

static void test_ids(Arena *a) {
    t_begin("doc: an id round-trips through its own spelling");
    char *d = kb_id_make(a, 'D', 241);
    ASSERT_EQ_S(d, "D-241");
    ASSERT_EQ_I(kb_id_num(d, 'D'), 241);

    t_begin("doc: the prefix carries the kind, so a mismatch is not a number");
    /* §1.1: the prefix is what makes a reference self-describing in a search
     * result or a prompt. Reading C-99812 as a document would silently point
     * at the wrong entity. */
    ASSERT_EQ_I(kb_id_num("C-99812", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("C-99812", 'C'), 99812);

    t_begin("doc: anything that is not an id reads as none");
    /* Zero, not a negative sentinel — the contract `doc.h` states, and what
     * every caller compares against. It is safe because the counters start at
     * one, so no entity is ever numbered zero and "not an id" cannot collide
     * with a real one. */
    ASSERT_EQ_I(kb_id_num("", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("D-", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("D-x", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("D-12x", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("241", 'D'), 0);
    ASSERT_EQ_I(kb_id_num(NULL, 'D'), 0);
}

static Document sample(void) {
    Document d;
    memset(&d, 0, sizeof d);
    d.id = "D-1";
    d.source = "S-1";
    d.path = "";
    d.title = "io_uring notes";
    d.mime = "text/markdown";
    d.content_hash = "cafebabe";
    d.fetched_at = "2026-01-01T00:00:00Z";
    d.indexed_at = "2026-01-01T00:00:00Z";
    d.bytes = 57;
    d.chunk_count = 2;
    d.chunk_base = 1;
    return d;
}

static void test_encoding(Arena *a) {
    t_begin("doc: a record is one line, and carries its fields");
    Document d = sample();
    size_t n = 0;
    char *line = doc_encode_document(a, &d, &n);
    ASSERT_TRUE(line != NULL);
    ASSERT_EQ_I((int64_t)n, (int64_t)strlen(line));
    ASSERT_TRUE(memchr(line, '\n', n) == NULL);
    ASSERT_TRUE(strstr(line, "\"D-1\"") != NULL);
    ASSERT_TRUE(strstr(line, "io_uring notes") != NULL);

    t_begin("doc: a newline in a title cannot break the record into two");
    /* The whole framing rests on this. A title is fetched from somewhere
     * else; if it could carry a raw newline it could append a line of its own
     * choosing to an append-only log that is supposed to be the truth. */
    d.title = "first\nsecond";
    line = doc_encode_document(a, &d, &n);
    ASSERT_TRUE(memchr(line, '\n', n) == NULL);
    ASSERT_TRUE(strstr(line, "\\n") != NULL);

    t_begin("doc: a quote in a title cannot close the string it is in");
    d.title = "he said \"hi\"";
    line = doc_encode_document(a, &d, &n);
    ASSERT_TRUE(memchr(line, '\n', n) == NULL);
    ASSERT_TRUE(strstr(line, "\\\"") != NULL);

    t_begin("doc: the chunk range is recorded, because the ids are public");
    /* §1.1 says a chunk id is never reused, and §1.6 says index/ is
     * disposable — so the range has to live in the log or a rebuild cannot
     * reproduce the ids it handed out. */
    d = sample();
    line = doc_encode_document(a, &d, &n);
    ASSERT_TRUE(strstr(line, "chunkBase") != NULL);
    ASSERT_TRUE(strstr(line, "chunkCount") != NULL);
}

static void test_touch(Arena *a) {
    t_begin("doc: an unchanged re-ingest appends a touch, not a document");
    /* §2: ingest is idempotent by content hash — the same text at the same
     * locator re-indexes nothing and updates fetchedAt. Writing a whole
     * document record instead would grow the log without saying anything new
     * and would reserve a fresh chunk range for chunks that did not change. */
    size_t n = 0;
    char *line = doc_encode_touch(a, "D-1", "2026-02-02T00:00:00Z", &n);
    ASSERT_TRUE(line != NULL);
    ASSERT_TRUE(memchr(line, '\n', n) == NULL);
    ASSERT_TRUE(strstr(line, "D-1") != NULL);
    ASSERT_TRUE(strstr(line, "2026-02-02T00:00:00Z") != NULL);
    /* It must be distinguishable from a document record, or a reader folding
     * the log cannot tell a re-fetch from a new version. */
    ASSERT_TRUE(strstr(line, "touch") != NULL);
}

static void test_source(Arena *a) {
    t_begin("doc: a source record survives a locator with the awkward bytes");
    Source s;
    memset(&s, 0, sizeof s);
    s.id = "S-1";
    s.kind = "url";
    s.locator = "https://example.invalid/a\"b\\c";
    s.title = "t";
    s.collection = "io-uring";
    /* Source stores only what cannot change. fetchedAt, contentHash,
     * docCount and bytes are §1.2 fields but §1.6 makes this log append-only,
     * so they are derived from the source's documents at read time rather
     * than re-appended — two records that disagreed would have no tiebreak. */
    s.created_at = "2026-01-01T00:00:00Z";
    size_t n = 0;
    char *line = doc_encode_source(a, &s, &n);
    ASSERT_TRUE(line != NULL);
    ASSERT_TRUE(memchr(line, '\n', n) == NULL);
    ASSERT_TRUE(strstr(line, "S-1") != NULL);
    ASSERT_TRUE(strstr(line, "io-uring") != NULL);
}

void test_doc(void) {
    Arena *a = arena_new(1 << 16);
    test_ids(a);
    test_encoding(a);
    test_touch(a);
    test_source(a);
    arena_free(a);
}
