#include "fts.h"

#include "errdet.h"
#include "sha256.h"

#include <math.h>

/* ---- the on-disk image -------------------------------------------------
 *
 * Offsets are absolute from the start of the file; sections are padded to
 * an 8-byte boundary and the padding is zeroed, so the same inputs always
 * produce byte-identical files. That is what lets `kb rebuild` be proved
 * to reconstruct exactly what was there before.
 */

#define FTS_MAGIC "KBFTSIDX"
#define FTS_MAGIC_LEN 8u
#define FTS_HEADER 192u
/* Reads back as 0x01020304 only when the file was written little-endian by
 * a build that agrees with this one about what an integer looks like. */
#define FTS_ENDIAN 0x01020304u

#define FTS_DOC_REC 24u
#define FTS_CHUNK_REC 12u
#define FTS_TERM_REC 16u
#define FTS_POST_REC 8u

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static uint64_t align8(uint64_t v) {
    return (v + 7u) & ~(uint64_t)7u;
}

/* ---- the builder's term map -------------------------------------------- */

typedef struct {
    const char *text;
    uint32_t len;
    uint32_t *post; /* (chunk, tf) pairs */
    uint32_t np, cap;
} BTerm;

typedef struct {
    Arena *a;
    BTerm *v;
    uint32_t n, cap;
    uint32_t *slots; /* 1-based index into v; 0 is empty */
    uint32_t slot_cap;
} TermMap;

static void tm_init(TermMap *m, Arena *a) {
    memset(m, 0, sizeof(*m));
    m->a = a;
    m->slot_cap = 1024;
    m->slots = (uint32_t *)arena_alloc0(a, m->slot_cap * sizeof(uint32_t));
}

static uint32_t tm_slot(const uint32_t *slots, uint32_t cap, const BTerm *v,
                        const char *text, uint32_t len) {
    uint32_t i = (uint32_t)str_hash(str_n(text, len)) & (cap - 1);
    while (slots[i]) {
        const BTerm *t = &v[slots[i] - 1];
        if (t->len == len && memcmp(t->text, text, len) == 0)
            break;
        i = (i + 1) & (cap - 1);
    }
    return i;
}

static uint32_t tm_intern(TermMap *m, const char *text, uint32_t len) {
    if (m->n * 2 >= m->slot_cap) {
        uint32_t nc = m->slot_cap * 2;
        uint32_t *ns = (uint32_t *)arena_alloc0(m->a, nc * sizeof(uint32_t));
        for (uint32_t i = 0; i < m->slot_cap; i++) {
            if (!m->slots[i])
                continue;
            const BTerm *t = &m->v[m->slots[i] - 1];
            ns[tm_slot(ns, nc, m->v, t->text, t->len)] = m->slots[i];
        }
        m->slots = ns;
        m->slot_cap = nc;
    }
    uint32_t s = tm_slot(m->slots, m->slot_cap, m->v, text, len);
    if (m->slots[s])
        return m->slots[s] - 1;
    size_t n = m->n, cap = m->cap;
    ARENA_GROW(m->a, m->v, n, cap, BTerm);
    m->cap = (uint32_t)cap;
    BTerm *t = &m->v[m->n];
    memset(t, 0, sizeof(*t));
    t->text = arena_strndup(m->a, text, len);
    t->len = len;
    m->slots[s] = ++m->n;
    return m->n - 1;
}

typedef struct {
    TermMap *tm;
    uint32_t chunk_index;
    uint32_t length;
    uint64_t postings;
} ScanCtx;

static void scan_term(const Token *t, void *ud) {
    ScanCtx *c = (ScanCtx *)ud;
    if (t->primary)
        c->length++;
    uint32_t ti = tm_intern(c->tm, t->text, t->len);
    BTerm *bt = &c->tm->v[ti];
    /* Chunks are scanned in ascending order and a term's postings are only
     * ever appended, so the list is sorted by construction and a repeat of
     * the same term in the same chunk is the tail entry. */
    if (bt->np && bt->post[(bt->np - 1) * 2] == c->chunk_index) {
        bt->post[(bt->np - 1) * 2 + 1]++;
        return;
    }
    if (bt->np == bt->cap) {
        uint32_t nc = bt->cap ? bt->cap * 2 : 4;
        bt->post = (uint32_t *)arena_realloc(
            c->tm->a, bt->post, (size_t)bt->cap * 2 * sizeof(uint32_t),
            (size_t)nc * 2 * sizeof(uint32_t));
        bt->cap = nc;
    }
    bt->post[bt->np * 2] = c->chunk_index;
    bt->post[bt->np * 2 + 1] = 1;
    bt->np++;
    c->postings++;
}

/* Lexicographic by bytes, shorter first on a common prefix — the order a
 * binary search at query time assumes. */
static int term_cmp(const void *x, const void *y) {
    const BTerm *a = (const BTerm *)x, *b = (const BTerm *)y;
    uint32_t n = a->len < b->len ? a->len : b->len;
    int c = n ? memcmp(a->text, b->text, n) : 0;
    if (c)
        return c;
    return a->len < b->len ? -1 : (a->len > b->len ? 1 : 0);
}

char *fts_build(Arena *a, const FtsDocInput *in, size_t n, size_t target_bytes,
                size_t overlap_bytes, const char *digest, size_t *out_len,
                FtsBuildStats *stats, char *err, size_t errsz) {
    TermMap tm;
    tm_init(&tm, a);
    FtsDoc *docs = (FtsDoc *)arena_alloc0(a, (n ? n : 1) * sizeof(FtsDoc));
    FtsChunk *chunks = NULL;
    size_t nchunks = 0, chunk_cap = 0;
    uint64_t total_len = 0, postings = 0;
    uint32_t mismatched = 0;

    for (size_t d = 0; d < n; d++) {
        /* The file's definitions, for the symbol field below. */
        SyntaxSymbol *syms = NULL;
        size_t nsyms = 0;
        if (in[d].lang == LANG_CODE && in[d].syn != SYNTAX_NONE)
            syntax_symbols(a, in[d].syn, in[d].text, in[d].len, &syms, &nsyms);
        Chunks ch = chunk_split(a, in[d].text, in[d].len, in[d].lang, in[d].syn,
                                target_bytes, overlap_bytes);
        /* Never past the range the log reserved: chunk ids are public and
         * are never reused (§1.1), and the id after this document's last is
         * already some other document's first. A count that disagrees means
         * the chunker changed under the store, which is what `POST /reindex`
         * is for; say so rather than mis-range the ids. */
        uint32_t keep = (uint32_t)ch.n;
        if (keep != in[d].chunk_count) {
            mismatched++;
            if (keep > in[d].chunk_count)
                keep = in[d].chunk_count;
        }
        docs[d].doc_num = in[d].doc_num;
        docs[d].chunk_base = in[d].chunk_base;
        docs[d].chunk_count = keep;
        docs[d].first_chunk = (uint32_t)nchunks;
        for (uint32_t i = 0; i < keep; i++) {
            ScanCtx c;
            c.tm = &tm;
            c.chunk_index = (uint32_t)nchunks;
            c.length = 0;
            c.postings = 0;
            /* A chunk is indexed under where it is, too, as boosted fields
             * (BM25F by repetition): the document's title three times, its
             * heading path and heading twice. The words that name a section
             * are the words a search for it uses, and the passage itself
             * seldom repeats them. */
            const char *fields[3] = {in[d].title, ch.v[i].context, ch.v[i].heading};
            const int32_t weight[3] = {3, 2, 2};
            for (int32_t f = 0; f < 3; f++)
                for (int32_t r = 0; fields[f] && r < weight[f]; r++)
                    token_scan(fields[f], strlen(fields[f]), scan_term, &c);
            /* And the names it defines, three times: a search for
             * `store_open` should land on the chunk that defines it before
             * the twenty that call it. */
            for (size_t k = 0; k < nsyms; k++) {
                if (syms[k].start < ch.v[i].start || syms[k].start >= ch.v[i].end)
                    continue;
                for (int32_t r = 0; r < 3; r++)
                    token_scan(syms[k].name, strlen(syms[k].name), scan_term, &c);
            }
            token_scan(in[d].text + ch.v[i].start, ch.v[i].end - ch.v[i].start,
                       scan_term, &c);
            postings += c.postings;
            ARENA_GROW(a, chunks, nchunks, chunk_cap, FtsChunk);
            chunks[nchunks].doc_index = (uint32_t)d;
            chunks[nchunks].ordinal = i;
            chunks[nchunks].length = c.length;
            total_len += c.length;
            nchunks++;
        }
    }

    if (tm.n)
        qsort(tm.v, tm.n, sizeof(BTerm), term_cmp);

    uint64_t strings_len = 0;
    for (uint32_t i = 0; i < tm.n; i++)
        strings_len += tm.v[i].len;

    uint64_t doc_off = FTS_HEADER;
    uint64_t chunk_off = align8(doc_off + (uint64_t)n * FTS_DOC_REC);
    uint64_t term_off = align8(chunk_off + (uint64_t)nchunks * FTS_CHUNK_REC);
    uint64_t post_off = align8(term_off + (uint64_t)tm.n * FTS_TERM_REC);
    uint64_t post_len = postings * FTS_POST_REC;
    uint64_t strings_off = post_off + post_len; /* postings keep the 8-align */
    uint64_t file_size = strings_off + strings_len;
    if (file_size > (uint64_t)SIZE_MAX) {
        snprintf(err, errsz, "index too large to build in memory");
        return NULL;
    }

    uint8_t *buf = (uint8_t *)arena_alloc0(a, (size_t)file_size);
    memcpy(buf, FTS_MAGIC, FTS_MAGIC_LEN);
    put_u32(buf + 8, KB_FTS_VERSION);
    put_u32(buf + 12, FTS_ENDIAN);
    put_u32(buf + 16, KB_TOKENIZER_VERSION);
    put_u32(buf + 20, KB_BM25_K1_MILLI);
    put_u32(buf + 24, KB_BM25_B_MILLI);
    put_u32(buf + 28, (uint32_t)n);
    put_u32(buf + 32, (uint32_t)nchunks);
    put_u32(buf + 36, tm.n);
    put_u64(buf + 48, total_len);
    put_u64(buf + 56, doc_off);
    put_u64(buf + 64, chunk_off);
    put_u64(buf + 72, term_off);
    put_u64(buf + 80, post_off);
    put_u64(buf + 88, post_len);
    put_u64(buf + 96, strings_off);
    put_u64(buf + 104, strings_len);
    put_u64(buf + 112, file_size);
    memcpy(buf + 120, digest, 64);

    for (size_t d = 0; d < n; d++) {
        uint8_t *p = buf + doc_off + (uint64_t)d * FTS_DOC_REC;
        put_u64(p, (uint64_t)docs[d].doc_num);
        put_u64(p + 8, (uint64_t)docs[d].chunk_base);
        put_u32(p + 16, docs[d].chunk_count);
        put_u32(p + 20, docs[d].first_chunk);
    }
    for (size_t i = 0; i < nchunks; i++) {
        uint8_t *p = buf + chunk_off + (uint64_t)i * FTS_CHUNK_REC;
        put_u32(p, chunks[i].doc_index);
        put_u32(p + 4, chunks[i].ordinal);
        put_u32(p + 8, chunks[i].length);
    }
    uint64_t po = 0, so = 0;
    for (uint32_t i = 0; i < tm.n; i++) {
        uint8_t *p = buf + term_off + (uint64_t)i * FTS_TERM_REC;
        put_u32(p, (uint32_t)so);
        put_u32(p + 4, tm.v[i].len);
        put_u32(p + 8, (uint32_t)po);
        put_u32(p + 12, tm.v[i].np);
        memcpy(buf + strings_off + so, tm.v[i].text, tm.v[i].len);
        so += tm.v[i].len;
        for (uint32_t k = 0; k < tm.v[i].np; k++) {
            uint8_t *q = buf + post_off + po + (uint64_t)k * FTS_POST_REC;
            put_u32(q, tm.v[i].post[k * 2]);
            put_u32(q + 4, tm.v[i].post[k * 2 + 1]);
        }
        po += (uint64_t)tm.v[i].np * FTS_POST_REC;
    }

    if (stats) {
        stats->chunks = (uint32_t)nchunks;
        stats->terms = tm.n;
        stats->mismatched = mismatched;
    }
    *out_len = (size_t)file_size;
    return (char *)buf;
}

/* ---- opening ----------------------------------------------------------- */

/* A section must lie wholly inside the file. Checked with subtraction so
 * that no addition can wrap, whatever the file claims. */
static bool section_ok(uint64_t off, uint64_t size, uint64_t file) {
    return off <= file && size <= file - off;
}

bool fts_open(Arena *a, const char *data, size_t len, FtsIndex *out,
              const char **code, char *err, size_t errsz) {
    const uint8_t *b = (const uint8_t *)data;
    memset(out, 0, sizeof(*out));
    *code = "index_stale";
    if (len < FTS_HEADER) {
        snprintf(err, errsz, "the index is %zu bytes; a header is %u", len,
                 FTS_HEADER);
        return false;
    }
    if (memcmp(b, FTS_MAGIC, FTS_MAGIC_LEN) != 0) {
        snprintf(err, errsz, "not a kb keyword index");
        return false;
    }
    uint32_t version = get_u32(b + 8);
    if (version != KB_FTS_VERSION) {
        snprintf(err, errsz,
                 "the index is format version %lu; this build reads %lu",
                 (unsigned long)version, (unsigned long)KB_FTS_VERSION);
        return false;
    }
    if (get_u32(b + 12) != FTS_ENDIAN) {
        snprintf(err, errsz, "the index was written with another byte order");
        return false;
    }
    uint32_t tokenizer = get_u32(b + 16);
    if (tokenizer != KB_TOKENIZER_VERSION) {
        snprintf(err, errsz,
                 "the index was built by tokeniser version %lu; this build "
                 "is %lu",
                 (unsigned long)tokenizer,
                 (unsigned long)KB_TOKENIZER_VERSION);
        return false;
    }
    uint64_t file_size = get_u64(b + 112);
    if (file_size != (uint64_t)len) {
        snprintf(err, errsz,
                 "the index says it is %llu bytes and is %zu — truncated",
                 (unsigned long long)file_size, len);
        return false;
    }

    out->version = version;
    out->tokenizer = tokenizer;
    out->k1_milli = get_u32(b + 20);
    out->b_milli = get_u32(b + 24);
    out->doc_count = get_u32(b + 28);
    out->chunk_count = get_u32(b + 32);
    out->term_count = get_u32(b + 36);
    out->total_len = get_u64(b + 48);
    memcpy(out->digest, b + 120, 64);
    out->digest[64] = '\0';
    out->file_bytes = file_size;

    uint64_t doc_off = get_u64(b + 56), chunk_off = get_u64(b + 64);
    uint64_t term_off = get_u64(b + 72), post_off = get_u64(b + 80);
    uint64_t post_len = get_u64(b + 88), strings_off = get_u64(b + 96);
    uint64_t strings_len = get_u64(b + 104);
    if (!section_ok(doc_off, (uint64_t)out->doc_count * FTS_DOC_REC,
                    file_size) ||
        !section_ok(chunk_off, (uint64_t)out->chunk_count * FTS_CHUNK_REC,
                    file_size) ||
        !section_ok(term_off, (uint64_t)out->term_count * FTS_TERM_REC,
                    file_size) ||
        !section_ok(post_off, post_len, file_size) ||
        !section_ok(strings_off, strings_len, file_size)) {
        snprintf(err, errsz, "the index's section table runs past its end");
        return false;
    }
    out->post = b + post_off;
    out->post_len = (size_t)post_len;
    out->strings = data + strings_off;
    out->strings_len = (size_t)strings_len;

    out->docs =
        (FtsDoc *)arena_alloc0(a, (out->doc_count + 1u) * sizeof(FtsDoc));
    for (uint32_t i = 0; i < out->doc_count; i++) {
        const uint8_t *p = b + doc_off + (uint64_t)i * FTS_DOC_REC;
        out->docs[i].doc_num = (int64_t)get_u64(p);
        out->docs[i].chunk_base = (int64_t)get_u64(p + 8);
        out->docs[i].chunk_count = get_u32(p + 16);
        out->docs[i].first_chunk = get_u32(p + 20);
        if (out->docs[i].first_chunk > out->chunk_count ||
            out->docs[i].chunk_count >
                out->chunk_count - out->docs[i].first_chunk) {
            snprintf(err, errsz, "document %lu names chunks that do not exist",
                     (unsigned long)i);
            return false;
        }
    }
    out->chunks =
        (FtsChunk *)arena_alloc0(a, (out->chunk_count + 1u) * sizeof(FtsChunk));
    for (uint32_t i = 0; i < out->chunk_count; i++) {
        const uint8_t *p = b + chunk_off + (uint64_t)i * FTS_CHUNK_REC;
        out->chunks[i].doc_index = get_u32(p);
        out->chunks[i].ordinal = get_u32(p + 4);
        out->chunks[i].length = get_u32(p + 8);
        if (out->chunks[i].doc_index >= out->doc_count) {
            snprintf(err, errsz, "chunk %lu names a document that does not "
                                 "exist",
                     (unsigned long)i);
            return false;
        }
    }
    out->terms =
        (FtsTerm *)arena_alloc0(a, (out->term_count + 1u) * sizeof(FtsTerm));
    for (uint32_t i = 0; i < out->term_count; i++) {
        const uint8_t *p = b + term_off + (uint64_t)i * FTS_TERM_REC;
        uint32_t s_off = get_u32(p), s_len = get_u32(p + 4);
        uint32_t p_off = get_u32(p + 8), df = get_u32(p + 12);
        /* Bounded here, once, so that every posting read during a search is
         * in bounds by construction rather than by re-checking. */
        if (!section_ok(s_off, s_len, strings_len) ||
            !section_ok(p_off, (uint64_t)df * FTS_POST_REC, post_len) ||
            df == 0) {
            snprintf(err, errsz, "term %lu points outside the index",
                     (unsigned long)i);
            return false;
        }
        out->terms[i].text = out->strings + s_off;
        out->terms[i].len = s_len;
        out->terms[i].post_off = p_off;
        out->terms[i].df = df;
    }
    *code = "";
    return true;
}

bool fts_load(Arena *a, const char *path, FtsIndex *out, const char **code,
              char *err, size_t errsz) {
    char *data;
    size_t len;
    memset(out, 0, sizeof(*out));
    *code = "index_stale";
    if (!plat_read_file_max(a, path, &data, &len, (size_t)-1)) {
        snprintf(err, errsz, "no keyword index at %s", path);
        return false;
    }
    return fts_open(a, data, len, out, code, err, errsz);
}

void fts_path(const Store *s, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", s->dir, KB_FTS_NAME);
}

/* ---- staleness --------------------------------------------------------- */

void fts_store_digest(const Store *s, ChunkParams cp, char out[65]) {
    Sha256 c;
    sha256_init(&c);
    char line[512];
    int32_t n = snprintf(line, sizeof line, "kb-fts/%lu tok/%lu bm25/%lu/%lu\n",
                         (unsigned long)KB_FTS_VERSION,
                         (unsigned long)KB_TOKENIZER_VERSION,
                         (unsigned long)KB_BM25_K1_MILLI,
                         (unsigned long)KB_BM25_B_MILLI);
    sha256_update(&c, line, (size_t)n);
    /* The chunker decides where every chunk starts and ends, so a change to
     * it invalidates the index exactly as a change to the tokeniser does. */
    n = snprintf(line, sizeof line, "chunker/%s/%lu/%lu\n", cp.chunker,
                 (unsigned long)cp.chunk_tokens,
                 (unsigned long)cp.chunk_overlap);
    sha256_update(&c, line, (size_t)n);
    for (size_t i = 0; i < s->documents.n; i++) {
        const Document *d = &s->documents.v[i];
        /* fetchedAt is deliberately absent: re-filing unchanged content is a
         * touch, and §2 says a touch re-indexes nothing. Title is absent
         * because it is not indexed and is read live from the log. mime and
         * path are present because they choose the splitter. */
        n = snprintf(line, sizeof line, "%s\t%s\t%lld\t%lu\t%s\t%s\n", d->id,
                     d->content_hash, (long long)d->chunk_base,
                     (unsigned long)d->chunk_count, d->mime ? d->mime : "",
                     d->path ? d->path : "");
        if (n < 0)
            continue;
        sha256_update(&c, line,
                      (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
    uint8_t digest[32];
    sha256_final(&c, digest);
    static const char hexd[] = "0123456789abcdef";
    for (int32_t i = 0; i < 32; i++) {
        out[i * 2] = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    out[64] = '\0';
}

/* §11's index_stale names the structures that need rebuilding. The keyword
 * index is the only one this file knows about. */
static void stale_details(const char *path) {
    static const char *const structures[] = {"keyword"};
    errdet_begin("index_stale");
    errdet_strs("structures", structures, 1);
    errdet_str("path", path);
}

bool fts_open_store(Arena *a, const Store *s, FtsIndex *out, const char **code,
                    char *err, size_t errsz) {
    /* An index over no documents is empty whatever is on disk, so a store
     * that has only ever been `kb init`ed is searchable immediately rather
     * than owing a rebuild to represent nothing. */
    if (s->documents.n == 0) {
        memset(out, 0, sizeof(*out));
        out->version = KB_FTS_VERSION;
        out->tokenizer = KB_TOKENIZER_VERSION;
        out->k1_milli = KB_BM25_K1_MILLI;
        out->b_milli = KB_BM25_B_MILLI;
        *code = "";
        return true;
    }
    char path[KB_PATH_MAX];
    fts_path(s, path, sizeof path);
    char why[512];
    if (!fts_load(a, path, out, code, why, sizeof why)) {
        snprintf(err, errsz, "store %s: fts.db %s (run \"kb rebuild\")",
                 s->dir, why);
        stale_details(path);
        return false;
    }
    ChunkParams cp = store_chunk_params(a, s);
    char want[65];
    fts_store_digest(s, cp, want);
    if (strcmp(want, out->digest) != 0 || out->doc_count != s->documents.n) {
        *code = "index_stale";
        snprintf(err, errsz,
                 "store %s: fts.db no longer describes the logs "
                 "(run \"kb rebuild\")",
                 s->dir);
        stale_details(path);
        return false;
    }
    /* The digest covers the documents in log order, so record i of the index
     * is document i of the log. Confirm it rather than trust it: everything
     * downstream reads the log through that correspondence. */
    for (uint32_t i = 0; i < out->doc_count; i++) {
        if (kb_id_num(s->documents.v[i].id, 'D') != out->docs[i].doc_num) {
            *code = "index_stale";
            snprintf(err, errsz,
                     "store %s: fts.db and the log disagree about "
                     "document %lu (run \"kb rebuild\")",
                     s->dir, (unsigned long)i);
            stale_details(path);
            return false;
        }
    }
    *code = "";
    return true;
}

/* ---- BM25 --------------------------------------------------------------
 *
 * Okapi BM25, the standard form:
 *
 *   idf(t)  = ln(1 + (N - df + 0.5) / (df + 0.5))
 *   w(t, D) = idf(t) · tf · (k1 + 1)
 *             ────────────────────────────────────
 *             tf + k1 · (1 - b + b · |D| / avgdl)
 *
 * The `1 +` inside the logarithm is not cosmetic. Without it a term that
 * appears in every chunk gets a NEGATIVE weight, and a document is punished
 * for containing a word the query asked for — a query for "the io_uring
 * ring" would rank a passage mentioning "the" below one that does not. With
 * it, such a term is worth almost nothing and never less than nothing,
 * which is the behaviour a reader expects and the reason every modern
 * implementation writes it this way.
 *
 * k1 saturates term frequency: the tenth occurrence of a word says far less
 * than the second. b, at 0.75, discounts long chunks, so a match inside a
 * two-line note counts for more than the same match buried in a manual
 * page. Both are recorded in the index header, so changing them is a change
 * the file itself reports.
 */

double fts_idf(const FtsIndex *ix, uint32_t df) {
    double n = (double)ix->chunk_count;
    double d = (double)df;
    return log(1.0 + (n - d + 0.5) / (d + 0.5));
}

static double avg_len(const FtsIndex *ix) {
    double avgdl =
        ix->chunk_count ? (double)ix->total_len / (double)ix->chunk_count : 1.0;
    return avgdl > 0.0 ? avgdl : 1.0;
}

/* The saturation-and-length half of the weight, with idf passed in rather
 * than computed. idf is a property of the TERM and the corpus, not of the
 * chunk, so a search that recomputed it would take a logarithm once per
 * posting instead of once per query term — the same number, tens of
 * thousands of times, for a term that is common enough to matter. */
static double bm25_weight(double k1, double b, double avgdl, double idf,
                          uint32_t tf, uint32_t chunk_len) {
    double f = (double)tf;
    double norm = k1 * (1.0 - b + b * (double)chunk_len / avgdl);
    return idf * (f * (k1 + 1.0)) / (f + norm);
}

double fts_term_score(const FtsIndex *ix, uint32_t df, uint32_t tf,
                      uint32_t chunk_len) {
    return bm25_weight((double)ix->k1_milli / 1000.0,
                       (double)ix->b_milli / 1000.0, avg_len(ix),
                       fts_idf(ix, df), tf, chunk_len);
}

/* Binary search over the sorted term table. */
static const FtsTerm *find_term(const FtsIndex *ix, const char *term) {
    size_t tlen = strlen(term);
    uint32_t lo = 0, hi = ix->term_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const FtsTerm *t = &ix->terms[mid];
        size_t n = t->len < tlen ? t->len : tlen;
        int c = n ? memcmp(t->text, term, n) : 0;
        if (c == 0)
            c = t->len < tlen ? -1 : (t->len > tlen ? 1 : 0);
        if (c == 0)
            return t;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

typedef struct {
    uint32_t chunk;
    double score;
} Acc;

static int hit_cmp(const void *x, const void *y) {
    const Acc *a = (const Acc *)x, *b = (const Acc *)y;
    if (a->score > b->score)
        return -1;
    if (a->score < b->score)
        return 1;
    /* A total order, so the same index and query always give the same list —
     * which is what makes "rebuild reproduces the previous answer" testable. */
    return a->chunk < b->chunk ? -1 : (a->chunk > b->chunk ? 1 : 0);
}

size_t fts_search(Arena *a, const FtsIndex *ix, const TermList *q,
                  double min_score, FtsDocFilter filter, void *ud,
                  size_t limit, FtsHit **out) {
    *out = NULL;
    if (ix->chunk_count == 0 || q->n == 0 || limit == 0)
        return 0;

    size_t nc = (size_t)ix->chunk_count;
    double *score = (double *)arena_alloc0(a, nc * sizeof(double));
    uint32_t *touched = (uint32_t *)arena_alloc(a, nc * sizeof(uint32_t));
    /* Separate from the score, because a scored chunk is not the same thing
     * as a chunk with a non-zero score and the list must hold each once. */
    uint8_t *seen = (uint8_t *)arena_alloc0(a, nc);
    size_t ntouched = 0;
    /* The filter is a property of a document, so it is asked once per
     * document however many of its chunks a term hits. */
    int8_t *pass = (int8_t *)arena_alloc0(a, ix->doc_count + 1u);

    double k1 = (double)ix->k1_milli / 1000.0;
    double b = (double)ix->b_milli / 1000.0;
    double avgdl = avg_len(ix);
    for (size_t i = 0; i < q->n; i++) {
        const FtsTerm *t = find_term(ix, q->v[i]);
        if (!t)
            continue;
        double idf = fts_idf(ix, t->df);
        for (uint32_t k = 0; k < t->df; k++) {
            const uint8_t *p =
                ix->post + t->post_off + (size_t)k * FTS_POST_REC;
            uint32_t c = get_u32(p);
            uint32_t tf = get_u32(p + 4);
            if (c >= ix->chunk_count)
                continue; /* the file said otherwise; never read through it */
            uint32_t di = ix->chunks[c].doc_index;
            if (filter) {
                if (!pass[di])
                    pass[di] = filter(di, ud) ? 1 : -1;
                if (pass[di] < 0)
                    continue;
            }
            if (!seen[c]) {
                seen[c] = 1;
                touched[ntouched++] = c;
            }
            score[c] += bm25_weight(k1, b, avgdl, idf, tf,
                                    ix->chunks[c].length);
        }
    }
    if (ntouched == 0)
        return 0;

    Acc *acc = (Acc *)arena_alloc(a, ntouched * sizeof(Acc));
    size_t n = 0;
    for (size_t i = 0; i < ntouched; i++) {
        if (score[touched[i]] < min_score)
            continue;
        acc[n].chunk = touched[i];
        acc[n].score = score[touched[i]];
        n++;
    }
    if (n == 0)
        return 0;
    qsort(acc, n, sizeof(Acc), hit_cmp);
    if (n > limit)
        n = limit;
    FtsHit *hits = (FtsHit *)arena_alloc(a, n * sizeof(FtsHit));
    for (size_t i = 0; i < n; i++) {
        hits[i].chunk_index = acc[i].chunk;
        hits[i].score = acc[i].score;
    }
    *out = hits;
    return n;
}
