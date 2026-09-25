#include "store.h"
#include "test.h"
#include "test_tmp.h"

#include "sha256.h"

#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

static char *jn(Arena *a, const char *base, const char *rest) {
    return arena_printf(a, "%s/%s", base, rest);
}

static bool append_doc(Store *s, const char *id, int64_t chunk_base,
                       uint32_t chunk_count) {
    Document d;
    memset(&d, 0, sizeof d);
    d.id = id;
    d.source = "S-1";
    d.path = "";
    d.title = "t";
    d.mime = "text/plain";
    d.content_hash = "cafe";
    d.fetched_at = "2026-01-01T00:00:00Z";
    d.indexed_at = "2026-01-01T00:00:00Z";
    d.chunk_base = chunk_base;
    d.chunk_count = chunk_count;
    size_t len;
    char *line = doc_encode_document(s->a, &d, &len);
    char err[512];
    return store_append(s, STORE_DOCUMENTS, line, len, err, sizeof err);
}

/* ---- discovery -------------------------------------------------------- */

static void test_discovery(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char cwd_before[KB_PATH_MAX];
    plat_getcwd(cwd_before, sizeof cwd_before);

    t_begin("store: discovery walks up from a deep directory");
    char err[512];
    char *store = jn(a, root, KB_DIR);
    ASSERT_TRUE(store_create(a, store, err, sizeof err));
    char *deep = jn(a, root, "a/b/c");
    ASSERT_TRUE(plat_mkdirs(deep));

    ASSERT_EQ_I(chdir(deep), 0);
    char found[KB_PATH_MAX];
    ASSERT_TRUE(store_find_project(found, sizeof found));
    /* The temp root may be reached through a symlink (macOS puts TMPDIR
     * under /var, which is a link to /private/var), so the path is matched
     * by the unique directory name rather than compared whole. */
    const char *leaf = strrchr(root, '/');
    ASSERT_TRUE(leaf && strstr(found, leaf + 1) != NULL);
    ASSERT_TRUE(strlen(found) >= strlen(KB_DIR) &&
                strcmp(found + strlen(found) - strlen(KB_DIR), KB_DIR) == 0);
    ASSERT_TRUE(plat_is_dir(found));
    char *sources = jn(a, found, KB_SOURCES_NAME);
    ASSERT_TRUE(plat_is_file(sources));

    t_begin("store: a .kb FILE does not stop the walk");
    /* A plain file named .kb in an intermediate directory is not a store;
     * the walk must pass it and keep going, or a stray file shadows a real
     * store above it. */
    char *decoy = jn(a, jn(a, root, "a"), KB_DIR);
    ASSERT_TRUE(plat_write_file_atomic(decoy, "not a store", 11));
    ASSERT_TRUE(plat_is_file(decoy));
    char found2[KB_PATH_MAX];
    ASSERT_TRUE(store_find_project(found2, sizeof found2));
    ASSERT_EQ_S(found2, found);

    t_begin("store: discovery gives up at the filesystem root");
    /* Nothing was initialized above TMPDIR, so the walk must run all the
     * way up and report nothing rather than loop or stop early. */
    char other[KB_PATH_MAX];
    tmp_dir(other, sizeof other);
    ASSERT_EQ_I(chdir(other), 0);
    char none[KB_PATH_MAX];
    ASSERT_TRUE(!store_find_project(none, sizeof none));

    chdir(cwd_before);
    tmp_rm(a, root);
    tmp_rm(a, other);
}

/* ---- identifiers ------------------------------------------------------ */

static void test_ids(Arena *a) {
    t_begin("store: id spelling round-trips and rejects the rest");
    ASSERT_EQ_I(kb_id_num("D-241", 'D'), 241);
    ASSERT_EQ_I(kb_id_num("S-3", 'S'), 3);
    ASSERT_EQ_I(kb_id_num("C-99812", 'C'), 99812);
    ASSERT_EQ_I(kb_id_num("D-241", 'S'), 0);
    ASSERT_EQ_I(kb_id_num("D241", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("D-", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("D-0", 'D'), 0);
    ASSERT_EQ_I(kb_id_num("D-12x", 'D'), 0);
    ASSERT_EQ_I(kb_id_num(NULL, 'D'), 0);
    ASSERT_EQ_S(kb_id_make(a, 'C', 99812), "C-99812");

    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *dir = jn(a, root, KB_DIR);
    char err[512];
    const char *code;
    ASSERT_TRUE(store_create(a, dir, err, sizeof err));

    Store s;
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    int64_t src = 0, doc = 0, chunk = 0;
    t_begin("store: ids are monotonic and allocated in ranges");
    ASSERT_TRUE(store_reserve(&s, 1, 1, 3, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_EQ_I(src, 1);
    ASSERT_EQ_I(doc, 1);
    ASSERT_EQ_I(chunk, 1);
    ASSERT_TRUE(append_doc(&s, "D-1", 1, 3));
    ASSERT_TRUE(store_reserve(&s, 0, 1, 2, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_EQ_I(doc, 2);
    ASSERT_EQ_I(chunk, 4); /* the first document's three are spent */
    ASSERT_TRUE(append_doc(&s, "D-2", 4, 2));
    ASSERT_TRUE(store_reserve(&s, 0, 1, 1, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_EQ_I(doc, 3);
    ASSERT_TRUE(append_doc(&s, "D-3", 6, 1));
    store_close(&s);

    t_begin("store: an id is not reused after its record is gone");
    /* Remove the newest document from the log entirely — the most that any
     * delete could ever take away. The counter is durable, so the next id
     * is still 4. */
    char *docs_path = jn(a, dir, KB_DOCUMENTS_NAME);
    char *data;
    size_t len;
    ASSERT_TRUE(plat_read_file_max(a, docs_path, &data, &len, (size_t)-1));
    size_t cut = len - 1; /* the final newline */
    while (cut > 0 && data[cut - 1] != '\n')
        cut--;
    ASSERT_TRUE(plat_truncate(docs_path, (uint64_t)cut));
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    ASSERT_EQ_I(s.documents.n, 2);
    ASSERT_TRUE(store_reserve(&s, 0, 1, 0, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_EQ_I(doc, 4);
    store_close(&s);

    t_begin("store: counters that fall behind the logs are floored by them");
    /* The counters live under index/, which is disposable. Losing them must
     * never let a live id come back. */
    char *counters = jn(a, dir, KB_COUNTERS_NAME);
    ASSERT_TRUE(plat_remove_file(counters));
    const char *planted =
        "{\"type\":\"document\",\"id\":\"D-99\",\"source\":\"S-1\","
        "\"path\":\"\",\"contentHash\":\"x\",\"chunkBase\":500,"
        "\"chunkCount\":7}\n";
    ASSERT_TRUE(tmp_append_raw(docs_path, planted, strlen(planted)));
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    ASSERT_EQ_I(s.next_document, 100);
    ASSERT_EQ_I(s.next_chunk, 507);
    store_close(&s);

    tmp_rm(a, root);
}

/* ---- the lock --------------------------------------------------------- */

static void test_lock(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *dir = jn(a, root, KB_DIR);
    char err[512];
    const char *code;
    ASSERT_TRUE(store_create(a, dir, err, sizeof err));

    t_begin("store: a second writer is refused and told who holds the lock");
    Store held;
    ASSERT_TRUE(store_open(a, &held, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    Store second;
    char err2[512];
    const char *code2 = "";
    ASSERT_TRUE(!store_open(a, &second, dir, TIER_PROJECT, true, err2,
                            sizeof err2, &code2));
    ASSERT_EQ_S(code2, "store_locked");
    char pid[32];
    snprintf(pid, sizeof pid, "%lld", (long long)plat_pid());
    ASSERT_TRUE(strstr(err2, pid) != NULL);

    t_begin("store: a reader is never blocked by a writer");
    Store reader;
    ASSERT_TRUE(store_open(a, &reader, dir, TIER_PROJECT, false, err,
                           sizeof err, &code));

    t_begin("store: an append without the lock is refused");
    /* Every append goes through the lock. A caller that reached this
     * function without it has a bug, and a quiet race is the worst way to
     * find out. */
    ASSERT_TRUE(!store_append(&reader, STORE_DOCUMENTS, "{}", 2, err,
                              sizeof err));
    ASSERT_TRUE(strstr(err, "lock") != NULL);
    int64_t src = 0, doc = 0, chunk = 0;
    ASSERT_TRUE(
        !store_reserve(&reader, 0, 1, 0, &src, &doc, &chunk, err, sizeof err));
    store_close(&reader);

    store_close(&held);
    t_begin("store: the lock is released when its holder closes");
    ASSERT_TRUE(store_open(a, &second, dir, TIER_PROJECT, true, err,
                           sizeof err, &code));
    store_close(&second);

    tmp_rm(a, root);
}

/* ---- crash mid-append ------------------------------------------------- */

static void test_torn_append(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *dir = jn(a, root, KB_DIR);
    char err[512];
    const char *code;
    ASSERT_TRUE(store_create(a, dir, err, sizeof err));
    char *docs_path = jn(a, dir, KB_DOCUMENTS_NAME);

    Store s;
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    int64_t src = 0, doc = 0, chunk = 0;
    ASSERT_TRUE(store_reserve(&s, 0, 1, 2, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_TRUE(append_doc(&s, "D-1", 1, 2));
    ASSERT_TRUE(store_reserve(&s, 0, 1, 2, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_TRUE(append_doc(&s, "D-2", 3, 2));

    /* The crash: the id for D-3 is reserved and made durable, then the
     * process dies with only part of the record on disk. */
    ASSERT_TRUE(store_reserve(&s, 0, 1, 2, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_EQ_I(doc, 3);
    const char *fragment = "{\"type\":\"document\",\"id\":\"D-3\",\"sou";
    ASSERT_TRUE(tmp_append_raw(docs_path, fragment, strlen(fragment)));
    store_close(&s); /* process death */

    t_begin("store: a reader survives a torn final line");
    Store r;
    ASSERT_TRUE(store_open(a, &r, dir, TIER_PROJECT, false, err, sizeof err,
                           &code));
    ASSERT_EQ_I(r.documents.n, 2);
    ASSERT_TRUE(r.documents.torn_tail);
    store_close(&r);

    t_begin("store: the next writer truncates the fragment away");
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    ASSERT_TRUE(!s.documents.torn_tail);
    ASSERT_EQ_I(s.documents.n, 2);

    t_begin("store: the interrupted record's id is never handed out again");
    ASSERT_TRUE(store_reserve(&s, 0, 1, 2, &src, &doc, &chunk, err,
                              sizeof err));
    ASSERT_EQ_I(doc, 4);
    ASSERT_TRUE(append_doc(&s, "D-4", 7, 2));
    store_close(&s);

    t_begin("store: the log reads cleanly after the repaired append");
    /* Without the repair the new record would be glued to the fragment and
     * would make one terminated, unparseable line. */
    DocList l;
    ASSERT_TRUE(doclog_load(a, docs_path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 3);
    ASSERT_TRUE(!l.torn_tail);
    ASSERT_EQ_S(l.v[2].id, "D-4");

    tmp_rm(a, root);
}

/* ---- blobs ------------------------------------------------------------ */

static void test_blobs(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *dir = jn(a, root, KB_DIR);
    char err[512];
    const char *code;
    ASSERT_TRUE(store_create(a, dir, err, sizeof err));
    Store s;
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));

    t_begin("store: a blob is stored under the hash of its own bytes");
    static const char body[] = "io_uring_prep_recv queues a receive.\n";
    char hash[65];
    bool written = false;
    ASSERT_TRUE(store_put_blob(&s, body, sizeof body - 1, hash, &written, err,
                               sizeof err));
    ASSERT_TRUE(written);
    char expect[65];
    sha256_hex(body, sizeof body - 1, expect);
    ASSERT_EQ_S(hash, expect);
    char path[KB_PATH_MAX];
    store_blob_path(&s, hash, path, sizeof path);
    ASSERT_TRUE(plat_is_file(path));
    /* Read the file back and re-hash it: the name must be the digest of
     * what is actually inside, not of what the caller passed. */
    char *back;
    size_t back_len;
    ASSERT_TRUE(plat_read_file_max(a, path, &back, &back_len, (size_t)-1));
    ASSERT_EQ_I(back_len, sizeof body - 1);
    char rehash[65];
    sha256_hex(back, back_len, rehash);
    ASSERT_EQ_S(rehash, hash);

    t_begin("store: re-storing identical content writes nothing");
    char hash2[65];
    bool written2 = true;
    ASSERT_TRUE(store_put_blob(&s, body, sizeof body - 1, hash2, &written2,
                               err, sizeof err));
    ASSERT_EQ_S(hash2, hash);
    ASSERT_TRUE(!written2);
    ASSERT_EQ_I(tmp_count_files(a, jn(a, dir, KB_BLOBS_NAME)), 1);

    t_begin("store: different content is a different blob");
    char hash3[65];
    ASSERT_TRUE(store_put_blob(&s, "other", 5, hash3, &written, err,
                               sizeof err));
    ASSERT_TRUE(written);
    ASSERT_TRUE(strcmp(hash3, hash) != 0);
    ASSERT_EQ_I(tmp_count_files(a, jn(a, dir, KB_BLOBS_NAME)), 2);

    t_begin("store: a blob reads back byte for byte");
    char *got;
    size_t got_len;
    ASSERT_TRUE(store_get_blob(&s, hash, &got, &got_len));
    ASSERT_EQ_I(got_len, sizeof body - 1);
    ASSERT_EQ_I(memcmp(got, body, sizeof body - 1), 0);

    t_begin("store: a blob name that is not a digest is refused");
    /* The name arrives from a log line, which is an editable file. */
    ASSERT_TRUE(!store_get_blob(&s, "../../etc/passwd", &got, &got_len));
    ASSERT_TRUE(!store_get_blob(&s, "", &got, &got_len));
    char not_hex[65];
    memset(not_hex, 'z', 64);
    not_hex[64] = '\0';
    ASSERT_TRUE(!store_get_blob(&s, not_hex, &got, &got_len));

    store_close(&s);
    tmp_rm(a, root);
}

/* ---- layout and chunk parameters -------------------------------------- */

static void test_layout(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *dir = jn(a, root, KB_DIR);
    char err[512];
    const char *code;

    t_begin("store: init writes the §1.6 layout");
    ASSERT_TRUE(store_create(a, dir, err, sizeof err));
    ASSERT_TRUE(plat_is_file(jn(a, dir, KB_SOURCES_NAME)));
    ASSERT_TRUE(plat_is_file(jn(a, dir, KB_DOCUMENTS_NAME)));
    ASSERT_TRUE(plat_is_dir(jn(a, dir, KB_BLOBS_NAME)));
    ASSERT_TRUE(plat_is_dir(jn(a, dir, KB_INDEX_NAME)));

    t_begin("store: .gitignore ignores index/ and nothing else");
    char *gi;
    size_t gi_len;
    ASSERT_TRUE(plat_read_file_max(a, jn(a, dir, KB_GITIGNORE_NAME), &gi,
                                   &gi_len, (size_t)-1));
    ASSERT_EQ_S(gi, "index/\n");

    t_begin("store: a second init refuses");
    ASSERT_TRUE(!store_create(a, dir, err, sizeof err));

    t_begin("store: chunking parameters are recorded once and kept");
    Store s;
    ASSERT_TRUE(store_open(a, &s, dir, TIER_PROJECT, true, err, sizeof err,
                           &code));
    ChunkParams before = store_chunk_params(a, &s);
    ASSERT_TRUE(!before.present);
    ASSERT_TRUE(store_write_chunk_params(&s, err, sizeof err));
    ChunkParams after = store_chunk_params(a, &s);
    ASSERT_TRUE(after.present);
    ASSERT_EQ_I(after.chunk_tokens, KB_CHUNK_TOKENS);
    ASSERT_EQ_I(after.chunk_overlap, KB_CHUNK_OVERLAP);
    ASSERT_EQ_S(after.chunker, KB_CHUNKER_ID);
    /* A store built with other parameters keeps saying so, which is the
     * only way a later reindex can tell they changed (§8). */
    const char *stale = "{\"chunker\":\"old\",\"chunkTokens\":128,"
                        "\"chunkOverlap\":16}\n";
    ASSERT_TRUE(plat_write_file_atomic(jn(a, dir, KB_MODEL_NAME), stale,
                                       strlen(stale)));
    ASSERT_TRUE(store_write_chunk_params(&s, err, sizeof err));
    ChunkParams kept = store_chunk_params(a, &s);
    ASSERT_EQ_I(kept.chunk_tokens, 128);
    ASSERT_EQ_S(kept.chunker, "old");
    store_close(&s);

    tmp_rm(a, root);
}

void test_store(void) {
    Arena *a = arena_new(1 << 16);
    test_discovery(a);
    test_ids(a);
    test_lock(a);
    test_torn_append(a);
    test_blobs(a);
    test_layout(a);
    arena_free(a);
}
