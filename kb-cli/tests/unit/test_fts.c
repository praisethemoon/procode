#include "fts.h"
#include "test.h"
#include "test_tmp.h"

/* Hand-computed BM25.
 *
 * Four one-chunk documents, each a handful of plain lower-case words with
 * no compound in them, so the terms are exactly the words and the lengths
 * are exactly the word counts:
 *
 *   d1  alpha beta gamma delta      |D| = 4
 *   d2  alpha alpha beta            |D| = 3
 *   d3  alpha gamma gamma gamma     |D| = 4
 *   d4  beta delta                  |D| = 2
 *
 *   N = 4, Σ|D| = 13, avgdl = 3.25, k1 = 1.2, b = 0.75
 *   df(alpha) = 3, df(beta) = 3, df(gamma) = 2, df(delta) = 2
 *
 *   idf(t)  = ln(1 + (N - df + 0.5)/(df + 0.5))
 *   w(t,D)  = idf(t) · tf·(k1+1) / (tf + k1·(1 - b + b·|D|/avgdl))
 *
 * Worked through, term by term:
 *
 *   idf(alpha) = ln(1 + 1.5/3.5)          = 0.3566749439
 *   idf(gamma) = idf(delta) = ln(1 + 1)   = 0.6931471806
 *
 *   query "alpha"        d1 0.3259074568  d2 0.5012728942  d3 0.3259074568
 *   query "alpha gamma"  d1 0.9592623885  d2 0.5012728942  d3 1.3638137062
 *   query "delta"        d1 0.6333549318  d4 0.8225730027
 *
 * The last line is the one that proves the length normalisation is there:
 * d1 and d4 each contain `delta` exactly once, and d4 wins only because it
 * is half as long. With b dropped to zero the two are identical.
 */

#define TOL 1e-9

static const char *const CORPUS_A[] = {
    "alpha beta gamma delta", "alpha alpha beta",
    "alpha gamma gamma gamma", "beta delta"};

static char *build(Arena *a, const char *const *texts, size_t n,
                   size_t *out_len, const char *digest) {
    FtsDocInput *in = (FtsDocInput *)arena_alloc0(a, n * sizeof(FtsDocInput));
    for (size_t i = 0; i < n; i++) {
        in[i].doc_num = (int64_t)i + 1;
        in[i].chunk_base = (int64_t)i + 1;
        in[i].chunk_count = 1;
        in[i].text = texts[i];
        in[i].len = strlen(texts[i]);
        in[i].lang = LANG_TEXT;
    }
    char err[512];
    FtsBuildStats stats;
    /* A window far larger than any of these documents, so each is one
     * chunk and the corpus is exactly the four lines above. */
    char *img = fts_build(a, in, n, 1u << 20, 0, digest, out_len, &stats, err,
                          sizeof err);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_I(stats.mismatched, 0);
    ASSERT_EQ_I(stats.chunks, n);
    return img;
}

/* A digest is 64 hex characters; what is in them does not matter here, and
 * spelling one out as a literal is a way to get the length wrong. */
static const char *fake_digest(Arena *a, char lead) {
    char *d = (char *)arena_alloc(a, 65);
    memset(d, '0', 64);
    d[0] = lead;
    d[64] = '\0';
    return d;
}

static bool open_built(Arena *a, const char *const *texts, size_t n,
                       FtsIndex *ix) {
    size_t len = 0;
    char *img = build(a, texts, n, &len, fake_digest(a, '0'));
    const char *code;
    char err[512];
    return img && fts_open(a, img, len, ix, &code, err, sizeof err);
}

static size_t run(Arena *a, const FtsIndex *ix, const char *q, FtsHit **hits) {
    TermList t = token_terms(a, q, strlen(q));
    return fts_search(a, ix, &t, 0.0, NULL, NULL, 100, hits);
}

/* A shorter result list than the test expected is a failure, not a crash.
 * A suite that segfaults loses the buffered lines naming what went wrong,
 * which is the one thing the run was for. */
static size_t nhits;

static double score_at(const FtsHit *h, size_t i) {
    return i < nhits ? h[i].score : -1.0;
}

static int64_t chunk_at(const FtsHit *h, size_t i) {
    return i < nhits ? (int64_t)h[i].chunk_index : -1;
}

/* ---- scoring ----------------------------------------------------------- */

static void test_bm25(Arena *a) {
    FtsIndex ix;
    t_begin("fts: the index records the corpus statistics BM25 needs");
    ASSERT_TRUE(open_built(a, CORPUS_A, 4, &ix));
    ASSERT_EQ_I(ix.doc_count, 4);
    ASSERT_EQ_I(ix.chunk_count, 4);
    ASSERT_EQ_I(ix.total_len, 13); /* 4 + 3 + 4 + 2 */
    ASSERT_EQ_I(ix.term_count, 4); /* alpha beta gamma delta */
    ASSERT_EQ_I(ix.k1_milli, KB_BM25_K1_MILLI);
    ASSERT_EQ_I(ix.b_milli, KB_BM25_B_MILLI);
    ASSERT_EQ_I(ix.chunks[0].length, 4);
    ASSERT_EQ_I(ix.chunks[1].length, 3);
    ASSERT_EQ_I(ix.chunks[3].length, 2);

    t_begin("fts: idf matches the hand-computed value");
    ASSERT_NEAR(fts_idf(&ix, 3), 0.3566749439, TOL);
    ASSERT_NEAR(fts_idf(&ix, 2), 0.6931471806, TOL);

    t_begin("fts: \"alpha\" scores exactly the hand-computed values");
    FtsHit *h;
    nhits = 0;
    size_t n = run(a, &ix, "alpha", &h);
    nhits = n;
    ASSERT_EQ_I(n, 3); /* d4 has no alpha */
    ASSERT_EQ_I(chunk_at(h, 0), 1);
    ASSERT_NEAR(score_at(h, 0), 0.5012728942, TOL);
    /* d1 and d3 score identically; the tie is broken by chunk index, so the
     * list is a function of the index and nothing else. */
    ASSERT_EQ_I(chunk_at(h, 1), 0);
    ASSERT_NEAR(score_at(h, 1), 0.3259074568, TOL);
    ASSERT_EQ_I(chunk_at(h, 2), 2);
    ASSERT_NEAR(score_at(h, 2), 0.3259074568, TOL);

    t_begin("fts: \"alpha gamma\" sums per-term weights, hand-computed");
    n = run(a, &ix, "alpha gamma", &h);
    nhits = n;
    ASSERT_EQ_I(n, 3);
    ASSERT_EQ_I(chunk_at(h, 0), 2);
    ASSERT_NEAR(score_at(h, 0), 1.3638137062, TOL);
    ASSERT_EQ_I(chunk_at(h, 1), 0);
    ASSERT_NEAR(score_at(h, 1), 0.9592623885, TOL);
    ASSERT_EQ_I(chunk_at(h, 2), 1);
    ASSERT_NEAR(score_at(h, 2), 0.5012728942, TOL);

    t_begin("fts: a shorter chunk wins on equal term frequency");
    /* The whole of length normalisation, in one assertion: d1 and d4 each
     * hold `delta` once, and d4 is the shorter passage. */
    n = run(a, &ix, "delta", &h);
    nhits = n;
    ASSERT_EQ_I(n, 2);
    ASSERT_EQ_I(chunk_at(h, 0), 3);
    ASSERT_NEAR(score_at(h, 0), 0.8225730027, TOL);
    ASSERT_EQ_I(chunk_at(h, 1), 0);
    ASSERT_NEAR(score_at(h, 1), 0.6333549318, TOL);
    ASSERT_TRUE(score_at(h, 0) > score_at(h, 1));

    t_begin("fts: term frequency saturates rather than accumulating");
    /* gamma three times in d3 is worth far less than three times gamma
     * once: 1.0379 against 3 × 0.6334. */
    n = run(a, &ix, "gamma", &h);
    nhits = n;
    ASSERT_EQ_I(chunk_at(h, 0), 2);
    ASSERT_NEAR(score_at(h, 0), 1.0379062494, TOL);
    ASSERT_TRUE(score_at(h, 0) < 3.0 * 0.6333549318);
}

static void test_universal_term(Arena *a) {
    /* A term in every chunk. With the `1 +` inside the logarithm its weight
     * is small and positive; without it the weight is ln(0.5/3.5), which is
     * negative, and a passage would be punished for containing a word the
     * query asked for. */
    static const char *const corpus[] = {"ubiquitous rare", "ubiquitous",
                                         "ubiquitous"};
    FtsIndex ix;
    t_begin("fts: a term in every chunk is worth little and never less than "
            "nothing");
    ASSERT_TRUE(open_built(a, corpus, 3, &ix));
    ASSERT_EQ_I(ix.chunk_count, 3);
    ASSERT_EQ_I(ix.total_len, 4);
    ASSERT_NEAR(fts_idf(&ix, 3), 0.1335313926, TOL);
    ASSERT_TRUE(fts_idf(&ix, 3) > 0.0);

    FtsHit *h;
    nhits = 0;
    size_t n = run(a, &ix, "ubiquitous", &h);
    nhits = n;
    ASSERT_EQ_I(n, 3);
    ASSERT_NEAR(score_at(h, 0), 0.1487438298, TOL);
    ASSERT_TRUE(score_at(h, 0) > 0.0);
    ASSERT_NEAR(score_at(h, 2), 0.1108562505, TOL);
    ASSERT_TRUE(score_at(h, 2) > 0.0);

    t_begin("fts: a rare term is worth many times a universal one");
    ASSERT_NEAR(fts_idf(&ix, 1), 0.9808292530, TOL);
    ASSERT_TRUE(fts_idf(&ix, 1) > 7.0 * fts_idf(&ix, 3));
    n = run(a, &ix, "rare", &h);
    nhits = n;
    ASSERT_EQ_I(n, 1);
    ASSERT_NEAR(score_at(h, 0), 0.8142733421, TOL);
}

static void test_exactness(Arena *a) {
    /* The identifier and the same four words scattered. Both chunks are
     * four primary tokens long, so nothing but the whole term separates
     * them — and the whole term is what a tokeniser that split on '_' would
     * not have. */
    static const char *const corpus[] = {
        "io_uring_prep_recv alpha beta gamma", "io uring prep recv"};
    FtsIndex ix;
    t_begin("fts: the exact identifier outranks its scattered words");
    ASSERT_TRUE(open_built(a, corpus, 2, &ix));
    ASSERT_EQ_I(ix.chunks[0].length, 4);
    ASSERT_EQ_I(ix.chunks[1].length, 4);
    FtsHit *h;
    nhits = 0;
    size_t n = run(a, &ix, "io_uring_prep_recv", &h);
    nhits = n;
    ASSERT_EQ_I(n, 2);
    ASSERT_EQ_I(chunk_at(h, 0), 0);
    ASSERT_TRUE(score_at(h, 0) > score_at(h, 1));

    t_begin("fts: recv and send are separated by the whole identifier");
    static const char *const pair[] = {"io_uring_prep_recv queues a receive",
                                       "io_uring_prep_send queues a send"};
    ASSERT_TRUE(open_built(a, pair, 2, &ix));
    n = run(a, &ix, "io_uring_prep_recv", &h);
    nhits = n;
    ASSERT_EQ_I(n, 2);
    ASSERT_EQ_I(chunk_at(h, 0), 0);
    /* Not a hair's difference: the exact term is in one chunk of two and
     * the shared parts are in both, so the gap is the whole idf of a term
     * with df = 1. */
    ASSERT_TRUE(score_at(h, 0) > score_at(h, 1) * 1.5);
}

/* ---- filters and limits ------------------------------------------------ */

static bool only_even(uint32_t doc_index, void *ud) {
    int32_t *calls = (int32_t *)ud;
    (*calls)++;
    return (doc_index % 2) == 0;
}

static void test_filter_and_limit(Arena *a) {
    FtsIndex ix;
    t_begin("fts: the document filter excludes chunks before they are ranked");
    ASSERT_TRUE(open_built(a, CORPUS_A, 4, &ix));
    TermList q = token_terms(a, "alpha", 5);
    FtsHit *h;
    int32_t calls = 0;
    size_t n = fts_search(a, &ix, &q, 0.0, only_even, &calls, 100, &h);
    nhits = n;
    ASSERT_EQ_I(n, 2); /* documents 0 and 2 */
    ASSERT_EQ_I(chunk_at(h, 0), 0);
    ASSERT_EQ_I(chunk_at(h, 1), 2);

    t_begin("fts: the filter is asked once per document, not once per hit");
    ASSERT_TRUE(calls <= 4);

    t_begin("fts: minScore is applied before the limit");
    n = fts_search(a, &ix, &q, 0.4, NULL, NULL, 100, &h);
    nhits = n;
    ASSERT_EQ_I(n, 1);
    ASSERT_NEAR(score_at(h, 0), 0.5012728942, TOL);

    t_begin("fts: the limit truncates the ranking, keeping the best");
    n = fts_search(a, &ix, &q, 0.0, NULL, NULL, 2, &h);
    nhits = n;
    ASSERT_EQ_I(n, 2);
    ASSERT_EQ_I(chunk_at(h, 0), 1);
    ASSERT_EQ_I(chunk_at(h, 1), 0);

    t_begin("fts: a query with no known term matches nothing");
    q = token_terms(a, "nonesuch", 8);
    ASSERT_EQ_I(fts_search(a, &ix, &q, 0.0, NULL, NULL, 100, &h), 0);
}

/* ---- the file format --------------------------------------------------- */

static void corrupt_check(Arena *a, const char *what, char *img, size_t len) {
    FtsIndex ix;
    const char *code = "";
    char err[512];
    t_begin(what);
    ASSERT_TRUE(!fts_open(a, img, len, &ix, &code, err, sizeof err));
    ASSERT_EQ_S(code, "index_stale");
}

static void test_format(Arena *a) {
    size_t len = 0;
    const char *digest = fake_digest(a, 'a');
    char *img = build(a, CORPUS_A, 4, &len, digest);
    FtsIndex ix;
    const char *code;
    char err[512];

    t_begin("fts: the header round-trips the stamps and the digest");
    ASSERT_TRUE(fts_open(a, img, len, &ix, &code, err, sizeof err));
    ASSERT_EQ_S(code, "");
    ASSERT_EQ_I(ix.version, KB_FTS_VERSION);
    ASSERT_EQ_I(ix.tokenizer, KB_TOKENIZER_VERSION);
    ASSERT_EQ_S(ix.digest, digest);
    ASSERT_EQ_I(ix.file_bytes, len);

    t_begin("fts: the same inputs always produce the same bytes");
    /* Without this, "rebuild reproduces what was there" is untestable and
     * a collaborator's index is not comparable to yours. */
    size_t len2 = 0;
    char *img2 = build(a, CORPUS_A, 4, &len2, digest);
    ASSERT_EQ_I(len2, len);
    ASSERT_EQ_I(memcmp(img, img2, len), 0);

    /* Each of these is a file a reader must refuse rather than misread. */
    char *copy = (char *)arena_alloc(a, len);

    memcpy(copy, img, len);
    copy[0] = 'X';
    corrupt_check(a, "fts: a file that is not an index is refused", copy, len);

    memcpy(copy, img, len);
    copy[8] = (char)(KB_FTS_VERSION + 1);
    corrupt_check(a, "fts: an index from another format version is refused",
                  copy, len);

    memcpy(copy, img, len);
    copy[16] = (char)(KB_TOKENIZER_VERSION + 1);
    corrupt_check(a, "fts: an index from another tokeniser is refused", copy,
                  len);

    memcpy(copy, img, len);
    copy[12] = 0x01; /* the byte-order probe, written the other way round */
    copy[13] = 0x02;
    copy[14] = 0x03;
    copy[15] = 0x04;
    corrupt_check(a, "fts: an index written with another byte order is "
                     "refused",
                  copy, len);

    memcpy(copy, img, len);
    corrupt_check(a, "fts: a truncated index is refused", copy, len - 1);
    corrupt_check(a, "fts: a file too short to hold a header is refused", copy,
                  4);

    memcpy(copy, img, len);
    copy[64] = (char)0xff; /* chunk section offset, low byte of a u64 */
    copy[65] = (char)0xff;
    copy[66] = (char)0xff;
    copy[67] = (char)0x7f;
    corrupt_check(a, "fts: a section table pointing past the end is refused",
                  copy, len);

    memcpy(copy, img, len);
    copy[72] = (char)0xff; /* term section offset */
    copy[73] = (char)0xff;
    copy[74] = (char)0xff;
    copy[75] = (char)0x7f;
    corrupt_check(a, "fts: a term table pointing past the end is refused", copy,
                  len);

    t_begin("fts: an index built from nothing is empty and valid");
    size_t elen = 0;
    char eerr[512];
    FtsBuildStats estats;
    char *empty = fts_build(a, NULL, 0, 1u << 20, 0, digest, &elen, &estats,
                            eerr, sizeof eerr);
    ASSERT_TRUE(empty != NULL);
    ASSERT_TRUE(fts_open(a, empty, elen, &ix, &code, err, sizeof err));
    ASSERT_EQ_I(ix.chunk_count, 0);
    FtsHit *h;
    ASSERT_EQ_I(run(a, &ix, "alpha", &h), 0);
}

/* ---- chunk ranges ------------------------------------------------------ */

static void test_chunk_ranges(Arena *a) {
    t_begin("fts: a document never indexes past the range the log reserved");
    /* The log says one chunk; the text splits into several. Handing out
     * chunkBase+1 would be handing out the NEXT document's identifier. */
    const char *big =
        "alpha alpha alpha alpha alpha alpha alpha alpha alpha alpha "
        "beta beta beta beta beta beta beta beta beta beta "
        "gamma gamma gamma gamma gamma gamma gamma gamma gamma gamma";
    FtsDocInput in[1];
    memset(in, 0, sizeof in);
    in[0].doc_num = 1;
    in[0].chunk_base = 1;
    in[0].chunk_count = 1; /* what the log reserved */
    in[0].text = big;
    in[0].len = strlen(big);
    in[0].lang = LANG_TEXT;
    size_t len = 0;
    FtsBuildStats stats;
    char err[512];
    char *img = fts_build(a, in, 1, 40, 0, /* a window far smaller */
                          fake_digest(a, '0'), &len, &stats, err, sizeof err);
    ASSERT_TRUE(img != NULL);
    ASSERT_EQ_I(stats.mismatched, 1);
    ASSERT_EQ_I(stats.chunks, 1);
    FtsIndex ix;
    const char *code;
    ASSERT_TRUE(fts_open(a, img, len, &ix, &code, err, sizeof err));
    ASSERT_EQ_I(ix.chunk_count, 1);
    ASSERT_EQ_I(ix.docs[0].chunk_count, 1);
}

/* ---- staleness against a real store ------------------------------------ */

static char *jn2(Arena *a, const char *base, const char *rest) {
    return arena_printf(a, "%s/%s", base, rest);
}

static void test_digest(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *dir = jn2(a, root, KB_DIR);
    char err[512];
    const char *code;
    ASSERT_TRUE(store_create(a, dir, err, sizeof err));

    Store s;
    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err, &code));
    ASSERT_TRUE(store_write_chunk_params(&s, err, sizeof err));
    Document d;
    memset(&d, 0, sizeof d);
    d.id = "D-1";
    d.source = "S-1";
    d.path = "";
    d.title = "t";
    d.mime = "text/plain";
    d.content_hash = "cafe";
    d.fetched_at = "2026-01-01T00:00:00Z";
    d.indexed_at = "2026-01-01T00:00:00Z";
    d.chunk_base = 1;
    d.chunk_count = 1;
    size_t len;
    char *line = doc_encode_document(a, &d, &len);
    ASSERT_TRUE(store_append(&s, STORE_DOCUMENTS, line, len, err, sizeof err));
    store_close(&s);

    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, false, err, sizeof err, &code));
    ChunkParams cp = store_chunk_params(a, &s);
    char digest_a[65], digest_b[65];
    fts_store_digest(&s, cp, digest_a);

    t_begin("fts: the digest is stable for an unchanged store");
    fts_store_digest(&s, cp, digest_b);
    ASSERT_EQ_S(digest_a, digest_b);
    ASSERT_EQ_I(strlen(digest_a), 64);
    store_close(&s);

    t_begin("fts: re-filing unchanged content does not invalidate the index");
    /* §2: a touch updates fetchedAt and re-indexes nothing. If fetchedAt
     * were in the digest, every touch would demand a rebuild. */
    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err, &code));
    char *touch = doc_encode_touch(a, "D-1", "2030-05-05T05:05:05Z", &len);
    ASSERT_TRUE(store_append(&s, STORE_DOCUMENTS, touch, len, err, sizeof err));
    store_close(&s);
    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, false, err, sizeof err, &code));
    ASSERT_EQ_S(s.documents.v[0].fetched_at, "2030-05-05T05:05:05Z");
    fts_store_digest(&s, store_chunk_params(a, &s), digest_b);
    ASSERT_EQ_S(digest_b, digest_a);
    store_close(&s);

    t_begin("fts: changed content does invalidate the index");
    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err, &code));
    d.content_hash = "beef";
    d.chunk_base = 2;
    line = doc_encode_document(a, &d, &len);
    ASSERT_TRUE(store_append(&s, STORE_DOCUMENTS, line, len, err, sizeof err));
    store_close(&s);
    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, false, err, sizeof err, &code));
    fts_store_digest(&s, store_chunk_params(a, &s), digest_b);
    ASSERT_TRUE(strcmp(digest_b, digest_a) != 0);

    t_begin("fts: a store whose index is missing reports index_stale");
    FtsIndex ix;
    ASSERT_TRUE(!fts_open_store(a, &s, &ix, &code, err, sizeof err));
    ASSERT_EQ_S(code, "index_stale");
    ASSERT_TRUE(strstr(err, "rebuild") != NULL);
    store_close(&s);

    t_begin("fts: changed chunking parameters invalidate the index");
    ASSERT_TRUE(
        store_open(a, &s, dir, TIER_PROJECT, false, err, sizeof err, &code));
    ChunkParams other = store_chunk_params(a, &s);
    other.chunk_tokens += 1;
    fts_store_digest(&s, other, digest_b);
    fts_store_digest(&s, store_chunk_params(a, &s), digest_a);
    ASSERT_TRUE(strcmp(digest_a, digest_b) != 0);
    store_close(&s);

    tmp_rm(a, root);
}

void test_fts(void) {
    Arena *a = arena_new(1 << 16);
    test_bm25(a);
    test_universal_term(a);
    test_exactness(a);
    test_filter_and_limit(a);
    test_format(a);
    test_chunk_ranges(a);
    test_digest(a);
    arena_free(a);
}
