#include "cmd.h"

#include "sha256.h"

/* POST /documents (§2): the caller already has the text and hands it over
 * rather than causing a second fetch. This is the load-bearing route — an
 * agent files what it just read as it goes, so the second question on the
 * same topic is answered from disk.
 */

static const char *const VALUE_FLAGS[] = {
    "--title", "--collection", "--url",  "--mime",
    "--file",  "--meta",       "--meta-file", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

/* Extension to mime, for the common documentation and source types. The
 * chunker also looks at the path, so this only has to be right often enough
 * to be useful — an explicit --mime always wins. */
static const char *mime_from_path(const char *path) {
    static const struct {
        const char *ext;
        const char *mime;
    } map[] = {{".md", "text/markdown"},        {".markdown", "text/markdown"},
               {".mdx", "text/markdown"},       {".html", "text/html"},
               {".htm", "text/html"},           {".txt", "text/plain"},
               {".c", "text/x-c"},              {".h", "text/x-c"},
               {".cc", "text/x-c++"},           {".cpp", "text/x-c++"},
               {".hpp", "text/x-c++"},          {".py", "text/x-python"},
               {".rs", "text/x-rust"},          {".go", "text/x-go"},
               {".java", "text/x-java"},        {".js", "text/javascript"},
               {".mjs", "text/javascript"},     {".ts", "application/typescript"},
               {".tsx", "application/typescript"}, {".json", "application/json"},
               {".sh", "application/x-sh"},     {".css", "text/x-css"},
               {".sql", "text/x-sql"},          {".tc", "text/x-typec"},
               {NULL, NULL}};
    if (!path || !path[0])
        return NULL;
    size_t n = strlen(path);
    for (int32_t i = 0; map[i].ext; i++) {
        size_t m = strlen(map[i].ext);
        if (n > m && strcmp(path + n - m, map[i].ext) == 0)
            return map[i].mime;
    }
    return NULL;
}

/* Metadata is a channel for whatever filed the document — a paper's authors
 * and year, a page's canonical URL, a symbol list. §1.2 calls it filterable
 * but not schema-bound, so the shape is checked and the contents never are:
 * kb has no vocabulary to check them against, and the moment it grew one
 * every caller would have to agree with it. */
static bool meta_arg(Arena *a, int32_t argc, char **argv, const char **out,
                     char *err, size_t errsz) {
    const char *inline_ = flag_value(argc, argv, VALUE_FLAGS, "--meta");
    const char *file = flag_value(argc, argv, VALUE_FLAGS, "--meta-file");
    *out = NULL;
    if (inline_ && file) {
        snprintf(err, errsz, "--meta and --meta-file are alternatives");
        return false;
    }
    const char *text = inline_;
    size_t len = inline_ ? strlen(inline_) : 0;
    if (file) {
        char *data;
        if (!read_text_arg(a, file, &data, &len)) {
            snprintf(err, errsz, "cannot read %s", file);
            return false;
        }
        text = data;
    }
    if (!text)
        return true;
    char jerr[256];
    JVal *v = json_parse(a, text, len, jerr, sizeof jerr);
    if (!v || v->t != J_OBJ) {
        snprintf(err, errsz, "metadata must be a JSON object (%s)",
                 v ? "got another kind of value" : jerr);
        return false;
    }
    /* The writer's own bytes, so what comes back out is what went in. */
    *out = arena_strndup(a, v->src.ptr, v->src.len);
    return true;
}

int32_t cmd_add(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *title = flag_value(argc, argv, VALUE_FLAGS, "--title");
    const char *collection =
        flag_value(argc, argv, VALUE_FLAGS, "--collection");
    const char *url = flag_value(argc, argv, VALUE_FLAGS, "--url");
    const char *file = flag_value(argc, argv, VALUE_FLAGS, "--file");
    const char *mime = flag_value(argc, argv, VALUE_FLAGS, "--mime");
    if (!title || !title[0]) {
        err_out(json, "usage", "kb add requires --title");
        return KB_EXIT_ERR;
    }
    if (!collection || !collection[0]) {
        err_out(json, "usage", "kb add requires --collection");
        return KB_EXIT_ERR;
    }
    if (strchr(collection, '/') || strchr(collection, '\\')) {
        /* Collections are a flat named scope (§1.3). A separator in the name
         * suggests nesting, which does not exist. */
        err_out(json, "usage", "collection names do not nest");
        return KB_EXIT_ERR;
    }
    char err[512];
    const char *meta = NULL;
    if (!meta_arg(a, argc, argv, &meta, err, sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }

    char *content;
    size_t content_len;
    if (file) {
        if (!read_text_arg(a, file, &content, &content_len)) {
            err_out(json, "not_found", "cannot read %s", file);
            return KB_EXIT_ERR;
        }
    } else if (!read_text_arg(a, "-", &content, &content_len)) {
        err_out(json, "usage", "cannot read content from stdin");
        return KB_EXIT_ERR;
    }
    if (content_len == 0) {
        err_out(json, "usage",
                "no content (pass --file or pipe the text on stdin)");
        return KB_EXIT_ERR;
    }

    /* kind and locator (§1.2). A url is what the caller says the text came
     * from; a --file with no url is located by its own path; content with
     * neither has no identity beyond itself, so it is located by its hash
     * and re-filing the same bytes is the same document. */
    char locator_buf[KB_PATH_MAX];
    const char *kind, *locator;
    char hash[65];
    sha256_hex(content, content_len, hash);
    bool have_file = file && strcmp(file, "-") != 0;
    if (url && url[0]) {
        kind = "url";
        locator = url;
    } else if (have_file) {
        if (!store_abs_path(file, locator_buf, sizeof locator_buf)) {
            err_out(json, "internal", "cannot resolve %s", file);
            return KB_EXIT_FATAL;
        }
        kind = "file";
        locator = locator_buf;
    } else {
        kind = "inline";
        snprintf(locator_buf, sizeof locator_buf, "inline:%s", hash);
        locator = locator_buf;
    }
    /* The locator names the document; the extension that reveals its type
     * can be on either the url or the file it was read from, and a url like
     * ".../iocp" has none while the file beside it is a .md. Take whichever
     * says something. */
    const char *path_hint = NULL;
    if (url && url[0] && mime_from_path(url))
        path_hint = url;
    else if (have_file && mime_from_path(file))
        path_hint = file;
    else
        path_hint = (url && url[0]) ? url : file;
    if (!mime)
        mime = mime_from_path(path_hint);
    if (!mime)
        mime = "text/plain";

    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    const char *code;
    /* Opened for write: the lock is taken, a torn tail from a previous crash
     * is truncated away, and the logs are loaded. Everything below decides
     * what to write from what it just read, inside that one locked section. */
    if (!store_open(a, &s, dir, true, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return strcmp(code, "internal") == 0 ? KB_EXIT_FATAL : KB_EXIT_ERR;
    }

    char now[32];
    plat_timestamp(now);

    const Source *existing_src =
        src_by_key(&s.sources, kind, locator, collection);
    const Document *existing_doc =
        existing_src ? doc_by_source_path(&s.documents, existing_src->id, "")
                     : NULL;

    Document d;
    /* Derived from what the record will carry, never from path_hint. The
     * hint is how the mime was guessed; it is not stored, and a reader that
     * only has the record has to reach the same splitter or the chunk range
     * in that record stops describing the chunks on disk (see doc_lang). */
    Lang lang = doc_lang(mime, "");
    bool created, reindexed, blob_written = false;

    if (existing_doc && strcmp(existing_doc->content_hash, hash) == 0) {
        /* §2: the same text at the same locator re-indexes nothing and
         * updates fetchedAt. In an append-only log that update is a later
         * record that supersedes the earlier one. */
        size_t len;
        char *line = doc_encode_touch(a, existing_doc->id, now, &len);
        if (!store_append(&s, STORE_DOCUMENTS, line, len, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
        d = *existing_doc;
        d.fetched_at = now;
        lang = doc_lang(d.mime, d.path);
        created = false;
        reindexed = false;
    } else {
        if (!store_put_blob(&s, content, content_len, hash, &blob_written, err,
                            sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }

        ChunkParams cp = store_chunk_params(a, &s);
        Chunks chunks =
            chunk_split(a, content, content_len, lang,
                        (size_t)cp.chunk_tokens * KB_BYTES_PER_TOKEN,
                        (size_t)cp.chunk_overlap * KB_BYTES_PER_TOKEN);

        /* Ids become durable here, before any record that uses them is
         * written. A changed document keeps its own id and takes a fresh
         * chunk range: the chunks it had are gone, and their ids go with
         * them rather than being handed to different text (§1.1). */
        int64_t src_n = 0, doc_n = 0, chunk_base = 0;
        if (!store_reserve(&s, existing_src ? 0 : 1, existing_doc ? 0 : 1,
                           (uint32_t)chunks.n, &src_n, &doc_n, &chunk_base,
                           err, sizeof err) ||
            !store_write_chunk_params(&s, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }

        const char *source_id;
        if (existing_src) {
            source_id = existing_src->id;
        } else {
            Source src;
            memset(&src, 0, sizeof src);
            src.id = kb_id_make(a, 'S', src_n);
            src.kind = kind;
            src.locator = locator;
            src.title = title;
            src.collection = collection;
            src.created_at = now;
            size_t slen;
            char *sline = doc_encode_source(a, &src, &slen);
            /* The source lands first: a crash between the two records leaves
             * a source with no documents, which reads as an empty source and
             * is reused by the next ingest of the same locator. The other
             * order would leave a document pointing at nothing. */
            if (!store_append(&s, STORE_SOURCES, sline, slen, err,
                              sizeof err)) {
                store_close(&s);
                err_out(json, "internal", "%s", err);
                return KB_EXIT_FATAL;
            }
            source_id = src.id;
        }

        memset(&d, 0, sizeof d);
        d.id = existing_doc ? existing_doc->id : kb_id_make(a, 'D', doc_n);
        d.source = source_id;
        d.path = "";
        d.title = title;
        d.mime = mime;
        d.content_hash = hash;
        d.bytes = (uint64_t)content_len;
        d.fetched_at = now;
        d.indexed_at = now;
        d.meta = meta;
        d.chunk_count = (uint32_t)chunks.n;
        d.chunk_base = chunk_base;
        size_t len;
        char *line = doc_encode_document(a, &d, &len);
        if (!store_append(&s, STORE_DOCUMENTS, line, len, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
        created = existing_doc == NULL;
        reindexed = true;
    }

    /* The keyword index is refreshed here, inside the same locked section
     * that just appended the record, so `kb search` answers correctly the
     * instant `kb add` returns and never asks a reader to run a maintenance
     * command first. It is a FULL rebuild of the store: the format is a
     * sorted image and merging one document into it in place would be a
     * second, subtler index-writing path to get wrong. The cost is one pass
     * over the store per ingest, which is the price of `POST /documents`
     * being the one-at-a-time route; the answer for bulk is
     * `POST /documents/batch` (§2) rebuilding once at the end, not an
     * incremental writer.
     *
     * A touch changes no text, so it changes no term and no length — the
     * digest deliberately excludes fetchedAt for exactly this reason — and
     * skipping the rebuild keeps re-filing unchanged content as cheap as §2
     * says it is. */
    if (reindexed) {
        uint32_t nd = 0, missing = 0;
        FtsBuildStats stats;
        /* The fold in memory is the one store_open read, which predates the
         * record just appended. Re-read it so the index — and the digest a
         * later search checks it against — describes the log as it now is
         * rather than as it was a moment ago. */
        if (!doclog_load(a, s.documents_path, &s.documents, err, sizeof err) ||
            !index_rebuild(a, &s, &nd, &missing, &stats, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
    }

    /* One shape for every outcome. A caller that has to work out which keys
     * are present before it can read the answer is doing the route's job. */
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb,
                  "{\"ok\":true,\"document\":\"%s\","
                  "\"source\":\"%s\",\"contentHash\":\"%s\",\"bytes\":%llu,",
                  d.id, d.source, d.content_hash,
                  (unsigned long long)d.bytes);
        sb_puts(&sb, "\"collection\":");
        json_escape_c(&sb, collection);
        sb_puts(&sb, ",\"mime\":");
        json_escape_c(&sb, d.mime ? d.mime : "");
        /* "splitter", not "chunker": index/model.json's `chunker` names the
         * whole algorithm and its parameters, this names which of its four
         * paths the document took. */
        sb_printf(&sb,
                  ",\"splitter\":\"%s\",\"chunkCount\":%lu,\"chunkBase\":%lld,"
                  "\"created\":%s,\"reindexed\":%s,\"blobWritten\":%s,"
                  "\"fetchedAt\":\"%s\"}",
                  chunk_lang_name(lang), (unsigned long)d.chunk_count,
                  (long long)d.chunk_base, created ? "true" : "false",
                  reindexed ? "true" : "false",
                  blob_written ? "true" : "false", now);
        puts(sb_finish(&sb));
    } else {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb, "%s  %s  ", d.id, d.source);
        sb_puts_safe(&sb, collection);
        sb_printf(&sb, "  %s  %lu chunk%s  %s", chunk_lang_name(lang),
                  (unsigned long)d.chunk_count, d.chunk_count == 1 ? "" : "s",
                  reindexed ? (created ? "(new)" : "(updated)")
                            : "(unchanged; fetchedAt updated)");
        puts(sb_finish(&sb));
    }
    store_close(&s);
    return KB_EXIT_OK;
}
