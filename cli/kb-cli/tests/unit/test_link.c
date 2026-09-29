/* §6's links and §7's collection delete, at the level where they are real:
 * records in the two append-only logs, and the fold that turns a sequence of
 * them into an adjacency.
 *
 * Everything under index/ is derived and there is deliberately no links.bin
 * (doc.h says why), so this fold IS the link layer. If it dropped an edge, or
 * kept one an `unlink` removed, no other structure would notice.
 */
#include "doc.h"
#include "store.h"
#include "test.h"
#include "test_tmp.h"

static char *jn(Arena *a, const char *base, const char *rest) {
    return arena_printf(a, "%s/%s", base, rest);
}

/* ---- the six ----------------------------------------------------------- */

static void test_types(void) {
    t_begin("links: §6's six are exactly the types that exist");
    ASSERT_EQ_I(LINK_TYPE_COUNT, 6);
    static const char *const want[] = {"supersedes", "cites", "analogue_of",
                                       "implements", "see_also", "imports",
                                       NULL};
    for (int32_t i = 0; want[i]; i++)
        ASSERT_EQ_S(LINK_TYPES[i], want[i]);
    ASSERT_TRUE(LINK_TYPES[LINK_TYPE_COUNT] == NULL);

    t_begin("links: an accepted type comes back as the table spells it");
    /* The table's own pointer, so a stored `rel` is the canonical spelling
     * and the fold can compare edges by pointer. */
    for (int32_t i = 0; LINK_TYPES[i]; i++)
        ASSERT_TRUE(link_type_canon(LINK_TYPES[i]) == LINK_TYPES[i]);

    t_begin("links: a type outside the five is not a link type");
    /* §6 names five. A sixth would make an edge claim a relationship the
     * store has no meaning for. */
    ASSERT_TRUE(link_type_canon("relates_to") == NULL);
    ASSERT_TRUE(link_type_canon("supersede") == NULL);
    ASSERT_TRUE(link_type_canon("supersedes ") == NULL);
    ASSERT_TRUE(link_type_canon("Supersedes") == NULL);
    ASSERT_TRUE(link_type_canon("SEE_ALSO") == NULL);
    ASSERT_TRUE(link_type_canon("see-also") == NULL);
    ASSERT_TRUE(link_type_canon("") == NULL);
    ASSERT_TRUE(link_type_canon(NULL) == NULL);
}

/* ---- the record -------------------------------------------------------- */

static void test_encoding(Arena *a) {
    Link l;
    l.from = "D-1";
    l.rel = "analogue_of";
    l.to = "D-7";
    l.created_at = "2026-09-20T12:34:56Z";

    t_begin("links: the record names its kind and carries all three fields");
    size_t len = 0;
    char *line = doc_encode_link(a, &l, true, &len);
    ASSERT_EQ_S(line, "{\"type\":\"link\",\"from\":\"D-1\","
                      "\"rel\":\"analogue_of\",\"to\":\"D-7\","
                      "\"createdAt\":\"2026-09-20T12:34:56Z\"}");
    ASSERT_EQ_I(len, (int64_t)strlen(line));

    t_begin("links: a removal is its own record kind, not a missing one");
    line = doc_encode_link(a, &l, false, &len);
    ASSERT_EQ_S(line, "{\"type\":\"unlink\",\"from\":\"D-1\","
                      "\"rel\":\"analogue_of\",\"to\":\"D-7\","
                      "\"createdAt\":\"2026-09-20T12:34:56Z\"}");

    t_begin("links: the record never contains a raw newline");
    /* The log's frame is '\n' and nothing else. A record that could hold one
     * would make a torn tail indistinguishable from two whole records. */
    ASSERT_TRUE(strchr(line, '\n') == NULL);

    t_begin("collections: a forget record names only the source it removes");
    line = doc_encode_source_forget(a, "S-3", &len);
    ASSERT_EQ_S(line, "{\"type\":\"forget\",\"id\":\"S-3\"}");
}

/* ---- the fold ---------------------------------------------------------- */

static bool put(const char *path, const char *line) {
    char buf[512];
    int32_t n = snprintf(buf, sizeof buf, "%s\n", line);
    return tmp_append_raw(path, buf, (size_t)n);
}

static void test_fold(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *path = jn(a, root, "documents.jsonl");
    char err[512];
    DocList l;

    t_begin("links: a link record becomes an edge and no document");
    ASSERT_TRUE(put(path, "{\"type\":\"document\",\"id\":\"D-1\",\"source\":"
                          "\"S-1\",\"path\":\"\",\"contentHash\":\"a\"}"));
    ASSERT_TRUE(put(path, "{\"type\":\"document\",\"id\":\"D-2\",\"source\":"
                          "\"S-1\",\"path\":\"b\",\"contentHash\":\"b\"}"));
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"cites\",\"to\":\"D-2\",\"createdAt\":\"t1\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 2);
    ASSERT_EQ_I(l.nlinks, 1);
    ASSERT_EQ_S(l.links[0].from, "D-1");
    ASSERT_EQ_S(l.links[0].rel, "cites");
    ASSERT_EQ_S(l.links[0].to, "D-2");
    ASSERT_EQ_S(l.links[0].created_at, "t1");

    t_begin("links: a link record hands out no identifier");
    /* It carries no `id`, so it must not floor the document counter and
     * cannot burn a D- or C- id. */
    ASSERT_EQ_I(l.max_id, 2);
    ASSERT_EQ_I(l.max_chunk_id, 0);

    t_begin("links: the triple is the identity, not the pair");
    /* Two types between the same two documents are two edges: IOCP cites
     * io_uring AND is an analogue of it are different statements. */
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"analogue_of\",\"to\":\"D-2\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, 2);

    t_begin("links: direction is part of the identity too");
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-2\",\"rel\":"
                          "\"cites\",\"to\":\"D-1\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, 3);
    ASSERT_TRUE(link_find(&l, "D-1", "cites", "D-2") != NULL);
    ASSERT_TRUE(link_find(&l, "D-2", "cites", "D-1") != NULL);

    t_begin("links: re-linking the same edge does not make a second one");
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"cites\",\"to\":\"D-2\",\"createdAt\":\"t9\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, 3);
    ASSERT_EQ_S(link_find(&l, "D-1", "cites", "D-2")->created_at, "t9");

    t_begin("links: an unlink removes exactly its own edge");
    ASSERT_TRUE(put(path, "{\"type\":\"unlink\",\"from\":\"D-1\",\"rel\":"
                          "\"cites\",\"to\":\"D-2\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, 2);
    ASSERT_TRUE(link_find(&l, "D-1", "cites", "D-2") == NULL);
    /* The other two survive: an unlink is not a wildcard. */
    ASSERT_TRUE(link_find(&l, "D-1", "analogue_of", "D-2") != NULL);
    ASSERT_TRUE(link_find(&l, "D-2", "cites", "D-1") != NULL);

    t_begin("links: an unlink of an edge that is not there changes nothing");
    ASSERT_TRUE(put(path, "{\"type\":\"unlink\",\"from\":\"D-9\",\"rel\":"
                          "\"cites\",\"to\":\"D-8\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, 2);

    t_begin("links: an edge can be restored after being removed");
    /* Append-only: the last record about a triple is what it means. */
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"cites\",\"to\":\"D-2\",\"createdAt\":\"t12\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, 3);
    ASSERT_EQ_S(link_find(&l, "D-1", "cites", "D-2")->created_at, "t12");

    t_begin("links: an edge to a document that is gone still folds");
    /* A link whose target was forgotten must not break a read. The edge is
     * kept and the far end is simply not resolvable. */
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"see_also\",\"to\":\"D-404\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_TRUE(link_find(&l, "D-1", "see_also", "D-404") != NULL);
    ASSERT_TRUE(doc_by_id(&l, "D-404") == NULL);

    t_begin("links: a record this build cannot mean anything by is dropped");
    /* A type outside the five, or a missing end. Half-applying either would
     * put an edge in the store that no writer could have made. */
    size_t before = l.nlinks;
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"relates_to\",\"to\":\"D-2\"}"));
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"to\":"
                          "\"D-2\"}"));
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"rel\":\"cites\",\"to\":"
                          "\"D-2\"}"));
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-1\",\"rel\":"
                          "\"cites\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.nlinks, (int64_t)before);

    t_begin("links: a build that does not know links reads the store anyway");
    /* The loader skips record kinds it has no meaning for, which is the
     * property that let links move into documents.jsonl at all. */
    ASSERT_TRUE(put(path, "{\"type\":\"something-later\",\"id\":\"D-3\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 2);

    tmp_rm(a, root);
}

/* ---- the source tombstone ---------------------------------------------- */

static void test_forget(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *path = jn(a, root, "sources.jsonl");
    char err[512];
    SourceList l;

    t_begin("collections: a forget record removes its source from the fold");
    ASSERT_TRUE(put(path, "{\"type\":\"source\",\"id\":\"S-1\",\"kind\":"
                          "\"inline\",\"locator\":\"x\",\"collection\":\"a\"}"));
    ASSERT_TRUE(put(path, "{\"type\":\"source\",\"id\":\"S-2\",\"kind\":"
                          "\"inline\",\"locator\":\"y\",\"collection\":\"b\"}"));
    ASSERT_TRUE(srclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 2);
    ASSERT_TRUE(put(path, "{\"type\":\"forget\",\"id\":\"S-1\"}"));
    ASSERT_TRUE(srclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 1);
    ASSERT_EQ_S(l.v[0].id, "S-2");

    t_begin("collections: a forgotten id is still an id that was handed out");
    /* §1.1: monotonic and never reused. The counter floors itself on max_id,
     * which is taken over raw records rather than over the fold. */
    ASSERT_EQ_I(l.max_id, 2);

    t_begin("collections: a rename is a later record for the same source");
    /* A collection lives nowhere but on a Source, so §7's PATCH is exactly a
     * revision folded last-wins. */
    ASSERT_TRUE(put(path, "{\"type\":\"source\",\"id\":\"S-2\",\"kind\":"
                          "\"inline\",\"locator\":\"y\",\"collection\":"
                          "\"renamed\"}"));
    ASSERT_TRUE(srclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 1);
    ASSERT_EQ_S(l.v[0].collection, "renamed");

    t_begin("collections: a source can come back after being forgotten");
    ASSERT_TRUE(put(path, "{\"type\":\"source\",\"id\":\"S-1\",\"kind\":"
                          "\"inline\",\"locator\":\"x\",\"collection\":\"c\"}"));
    ASSERT_TRUE(srclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 2);
    ASSERT_TRUE(src_by_id(&l, "S-1") != NULL);
    ASSERT_EQ_S(src_by_id(&l, "S-1")->collection, "c");

    t_begin("collections: forgetting a source that is not there is harmless");
    ASSERT_TRUE(put(path, "{\"type\":\"forget\",\"id\":\"S-99\"}"));
    ASSERT_TRUE(srclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 2);
    /* The id is still spent, even though nothing held it. */
    ASSERT_EQ_I(l.max_id, 99);

    tmp_rm(a, root);
}

/* ---- the document tombstone -------------------------------------------- */

static void test_document_forget(Arena *a) {
    char root[KB_PATH_MAX];
    tmp_dir(root, sizeof root);
    char *path = jn(a, root, "documents.jsonl");
    char err[512];
    DocList l;

    t_begin("forget: a forget record removes its document from the fold");
    ASSERT_TRUE(put(path, "{\"type\":\"document\",\"id\":\"D-1\",\"source\":"
                          "\"S-1\",\"path\":\"\",\"contentHash\":\"a\","
                          "\"chunkBase\":1,\"chunkCount\":3}"));
    ASSERT_TRUE(put(path, "{\"type\":\"document\",\"id\":\"D-2\",\"source\":"
                          "\"S-1\",\"path\":\"b\",\"contentHash\":\"b\","
                          "\"chunkBase\":4,\"chunkCount\":2}"));
    ASSERT_TRUE(put(path, "{\"type\":\"link\",\"from\":\"D-2\",\"rel\":"
                          "\"cites\",\"to\":\"D-1\"}"));
    size_t len = 0;
    char *line = doc_encode_document_forget(a, "D-1", &len);
    ASSERT_EQ_S(line, "{\"type\":\"forget\",\"id\":\"D-1\"}");
    ASSERT_TRUE(put(path, line));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 1);
    ASSERT_EQ_S(l.v[0].id, "D-2");
    ASSERT_TRUE(doc_by_id(&l, "D-1") == NULL);

    t_begin("forget: the id and its chunk range stay spent");
    /* §1.1: never reused. The next D- and C- ids come after everything ever
     * written, forgotten or not. */
    ASSERT_EQ_I(l.max_id, 2);
    ASSERT_EQ_I(l.max_chunk_id, 5);

    t_begin("forget: an edge to a forgotten document stays in the fold");
    /* §6: it reads as resolved:false rather than disappearing, so the
     * dangling edge is shown instead of hidden. */
    ASSERT_EQ_I(l.nlinks, 1);
    ASSERT_TRUE(link_find(&l, "D-2", "cites", "D-1") != NULL);

    t_begin("forget: a touch for a forgotten document brings nothing back");
    ASSERT_TRUE(put(path, "{\"type\":\"touch\",\"id\":\"D-1\","
                          "\"fetchedAt\":\"2026-01-01T00:00:00Z\"}"));
    ASSERT_TRUE(doclog_load(a, path, &l, err, sizeof err));
    ASSERT_EQ_I(l.n, 1);

    tmp_rm(a, root);
}

/* ---- blob names -------------------------------------------------------- */

static void test_blob_names(void) {
    t_begin("compact: one predicate decides what a blob name is");
    /* compact turns directory entries into paths to unlink, so the rule that
     * refuses "../../etc/passwd" for a read has to be the same rule. */
    char ok[65];
    memset(ok, 'a', 64);
    ok[64] = '\0';
    ASSERT_TRUE(store_is_blob_name(ok));
    memset(ok, '0', 64);
    ASSERT_TRUE(store_is_blob_name(ok));
    memset(ok, 'f', 64);
    ASSERT_TRUE(store_is_blob_name(ok));

    ASSERT_TRUE(!store_is_blob_name(NULL));
    ASSERT_TRUE(!store_is_blob_name(""));
    ASSERT_TRUE(!store_is_blob_name("../../etc/passwd"));
    ASSERT_TRUE(!store_is_blob_name("."));
    ASSERT_TRUE(!store_is_blob_name(".."));
    ASSERT_TRUE(!store_is_blob_name("model.json"));
    char upper[65];
    memset(upper, 'A', 64);
    upper[64] = '\0';
    ASSERT_TRUE(!store_is_blob_name(upper));
    char shorter[64];
    memset(shorter, 'a', 63);
    shorter[63] = '\0';
    ASSERT_TRUE(!store_is_blob_name(shorter));
    char longer[66];
    memset(longer, 'a', 65);
    longer[65] = '\0';
    ASSERT_TRUE(!store_is_blob_name(longer));
    char with_slash[65];
    memset(with_slash, 'a', 64);
    with_slash[32] = '/';
    with_slash[64] = '\0';
    ASSERT_TRUE(!store_is_blob_name(with_slash));
}

void test_link(void) {
    Arena *a = arena_new(1 << 16);
    test_types();
    test_encoding(a);
    test_fold(a);
    test_forget(a);
    test_document_forget(a);
    test_blob_names();
    arena_free(a);
}
