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
static const char *const BOOL_FLAGS[] = {"--json", "--batch", NULL};

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

/* ---- one document ------------------------------------------------------
 *
 * Filing is two halves, so `kb add` and `kb add --batch` take the same path:
 * `prepare` works out everything that needs no store — kind, locator, mime,
 * hash — and can refuse before anything is locked; `ingest` writes one
 * prepared document into a store its caller has open for write. Neither
 * rebuilds the keyword index: the caller does that once, after the last
 * document, which is the whole point of a batch.
 */

typedef struct {
    const char *title;
    const char *collection;
    const char *url;  /* NULL or "" when the caller named none */
    const char *file; /* the path the text was read from; NULL for stdin */
    const char *mime; /* NULL: inferred from the url or the file */
    const char *meta; /* a JSON object's text, or NULL */
    const char *content;
    size_t len;
    /* Filled by prepare. */
    const char *kind;
    const char *locator;
    char hash[65];
} Filing;

typedef struct {
    Document d;
    Lang lang;
    bool created, reindexed, blob_written;
} Filed;

/* Fails with *code "usage", or "unsupported_mime" for a type the chunker
 * cannot split. */
static bool prepare(Arena *a, Filing *f, const char **code, char *err,
                    size_t errsz) {
    *code = "usage";
    if (!f->title || !f->title[0]) {
        snprintf(err, errsz, "a document needs a title");
        return false;
    }
    if (!f->collection || !f->collection[0]) {
        snprintf(err, errsz, "a document needs a collection");
        return false;
    }
    if (strchr(f->collection, '/') || strchr(f->collection, '\\')) {
        /* Collections are a flat named scope (§1.3). A separator in the name
         * suggests nesting, which does not exist. */
        snprintf(err, errsz, "collection names do not nest");
        return false;
    }
    if (f->len == 0) {
        snprintf(err, errsz, "no content (pass --file or pipe the text on "
                             "stdin)");
        return false;
    }
    /* kind and locator (§1.2). A url is what the caller says the text came
     * from; a --file with no url is located by its own path; content with
     * neither has no identity beyond itself, so it is located by its hash
     * and re-filing the same bytes is the same document. */
    sha256_hex(f->content, f->len, f->hash);
    bool have_file = f->file && strcmp(f->file, "-") != 0;
    if (f->url && f->url[0]) {
        f->kind = "url";
        f->locator = f->url;
    } else if (have_file) {
        char buf[KB_PATH_MAX];
        if (!store_abs_path(f->file, buf, sizeof buf)) {
            snprintf(err, errsz, "cannot resolve %s", f->file);
            return false;
        }
        f->kind = "file";
        f->locator = arena_strdup(a, buf);
    } else {
        f->kind = "inline";
        f->locator = arena_printf(a, "inline:%s", f->hash);
    }
    /* The locator names the document; the extension that reveals its type
     * can be on either the url or the file it was read from, and a url like
     * ".../iocp" has none while the file beside it is a .md. Take whichever
     * says something. */
    const char *path_hint = NULL;
    if (f->url && f->url[0] && mime_from_path(f->url))
        path_hint = f->url;
    else if (have_file && mime_from_path(f->file))
        path_hint = f->file;
    else
        path_hint = (f->url && f->url[0]) ? f->url : f->file;
    if (!f->mime)
        f->mime = mime_from_path(path_hint);
    if (!f->mime)
        f->mime = "text/plain";
    if (!chunk_mime_supported(f->mime)) {
        *code = "unsupported_mime";
        errdet_begin("unsupported_mime");
        errdet_str("mime", f->mime);
        snprintf(err, errsz,
                 "%s is not text kb can split; extract the text and file it "
                 "as text/plain or text/markdown",
                 f->mime);
        return false;
    }
    return true;
}

static bool ingest(Arena *a, Store *s, const Filing *f, const char *now,
                   Filed *out, char *err, size_t errsz) {
    const Source *existing_src =
        src_by_key(&s->sources, f->kind, f->locator, f->collection);
    const Document *existing_doc =
        existing_src ? doc_by_source_path(&s->documents, existing_src->id, "")
                     : NULL;
    out->blob_written = false;
    if (existing_doc) {
        /* The same locator again: a touch when the text is unchanged, a new
         * version under the same id when it is not. */
        if (!refile_document(a, s, existing_doc, f->content, f->len, f->hash,
                             f->title, f->mime, f->meta, now, &out->d,
                             &out->reindexed, &out->blob_written, err, errsz))
            return false;
        out->lang = doc_lang(out->d.mime, out->d.path);
        out->created = false;
        return true;
    }
    if (!store_put_blob(s, f->content, f->len, (char *)f->hash,
                        &out->blob_written, err, errsz))
        return false;
    /* Derived from what the record will carry, never from the path hint. The
     * hint is how the mime was guessed; it is not stored, and a reader that
     * only has the record has to reach the same splitter or the chunk range
     * in that record stops describing the chunks on disk (see doc_lang). */
    out->lang = doc_lang(f->mime, "");
    ChunkParams cp = store_chunk_params(a, s);
    Chunks chunks = chunk_split(a, f->content, f->len, out->lang,
                                (size_t)cp.chunk_tokens * KB_BYTES_PER_TOKEN,
                                (size_t)cp.chunk_overlap * KB_BYTES_PER_TOKEN);
    /* Ids become durable here, before any record that uses them is
     * written. */
    int64_t src_n = 0, doc_n = 0, chunk_base = 0;
    if (!store_reserve(s, existing_src ? 0 : 1, 1, (uint32_t)chunks.n, &src_n,
                       &doc_n, &chunk_base, err, errsz) ||
        !store_write_chunk_params(s, err, errsz))
        return false;
    const char *source_id;
    if (existing_src) {
        source_id = existing_src->id;
    } else {
        Source src;
        memset(&src, 0, sizeof src);
        src.id = kb_id_make(a, 'S', src_n);
        src.kind = f->kind;
        src.locator = f->locator;
        src.title = f->title;
        src.collection = f->collection;
        src.created_at = now;
        size_t slen;
        char *sline = doc_encode_source(a, &src, &slen);
        /* The source lands first: a crash between the two records leaves a
         * source with no documents, which reads as an empty source and is
         * reused by the next ingest of the same locator. The other order
         * would leave a document pointing at nothing. */
        if (!store_append(s, STORE_SOURCES, sline, slen, err, errsz))
            return false;
        source_id = src.id;
    }
    Document *d = &out->d;
    memset(d, 0, sizeof *d);
    d->id = kb_id_make(a, 'D', doc_n);
    d->source = source_id;
    d->path = "";
    d->title = f->title;
    d->mime = f->mime;
    d->content_hash = arena_strdup(a, f->hash);
    d->bytes = (uint64_t)f->len;
    d->fetched_at = now;
    d->indexed_at = now;
    d->meta = f->meta;
    d->chunk_count = (uint32_t)chunks.n;
    d->chunk_base = chunk_base;
    size_t len;
    char *line = doc_encode_document(a, d, &len);
    if (!store_append(s, STORE_DOCUMENTS, line, len, err, errsz))
        return false;
    out->created = true;
    out->reindexed = true;
    return true;
}

/* Re-read the logs after a write, so the next document in a batch sees the
 * sources and documents the previous ones created — the same locator twice
 * in one batch is one document, as it would be across two `kb add`s — and so
 * the index is rebuilt from the log as it now is. */
static bool reread(Arena *a, Store *s, char *err, size_t errsz) {
    return doclog_load(a, s->documents_path, &s->documents, err, errsz) &&
           srclog_load(a, s->sources_path, &s->sources, err, errsz);
}

/* The keyword index is refreshed inside the same locked section that
 * appended the records, so `kb search` answers correctly the instant
 * `kb add` returns and never asks a reader to run a maintenance command
 * first. It is a FULL rebuild of the store: the format is a sorted image and
 * merging one document into it in place would be a second, subtler
 * index-writing path to get wrong. The cost is one pass over the store per
 * `kb add`, and one per batch, which is what the batch is for.
 *
 * A touch changes no text, so it changes no term and no length — the digest
 * deliberately excludes fetchedAt for exactly this reason — and skipping the
 * rebuild keeps re-filing unchanged content as cheap as §2 says it is. */
static bool rebuild(Arena *a, Store *s, char *err, size_t errsz) {
    uint32_t nd = 0, missing = 0;
    FtsBuildStats stats;
    return index_rebuild(a, s, &nd, &missing, &stats, err, errsz);
}

/* One answer's fields, the same for every outcome: a caller that has to work
 * out which keys are present before it can read the answer is doing the
 * route's job. */
static void filed_json(StrBuf *sb, const Filing *f, const Filed *r,
                       const char *now) {
    sb_printf(sb,
              "\"document\":\"%s\",\"source\":\"%s\",\"contentHash\":\"%s\","
              "\"bytes\":%llu,",
              r->d.id, r->d.source, r->d.content_hash,
              (unsigned long long)r->d.bytes);
    sb_puts(sb, "\"collection\":");
    json_escape_c(sb, f->collection);
    sb_puts(sb, ",\"mime\":");
    json_escape_c(sb, r->d.mime ? r->d.mime : "");
    /* "splitter", not "chunker": index/model.json's `chunker` names the
     * whole algorithm and its parameters, this names which of its four paths
     * the document took. */
    sb_printf(sb,
              ",\"splitter\":\"%s\",\"chunkCount\":%lu,\"chunkBase\":%lld,"
              "\"created\":%s,\"reindexed\":%s,\"blobWritten\":%s,"
              "\"fetchedAt\":\"%s\"",
              chunk_lang_name(r->lang), (unsigned long)r->d.chunk_count,
              (long long)r->d.chunk_base, r->created ? "true" : "false",
              r->reindexed ? "true" : "false",
              r->blob_written ? "true" : "false", now);
}

static void filed_human(Arena *a, const Filing *f, const Filed *r) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "%s  %s  ", r->d.id, r->d.source);
    sb_puts_safe(&sb, f->collection);
    sb_printf(&sb, "  %s  %lu chunk%s  %s", chunk_lang_name(r->lang),
              (unsigned long)r->d.chunk_count, r->d.chunk_count == 1 ? "" : "s",
              r->reindexed ? (r->created ? "(new)" : "(updated)")
                           : "(unchanged; fetchedAt updated)");
    puts(sb_finish(&sb));
}

static bool open_store(Arena *a, Store *s, bool json, int32_t *rc) {
    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        *rc = KB_EXIT_ERR;
        return false;
    }
    const char *code;
    /* Opened for write: the lock is taken, a torn tail from a previous crash
     * is truncated away, and the logs are loaded. Everything after decides
     * what to write from what it just read, inside that one locked section. */
    if (!store_open(a, s, dir, true, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        *rc = strcmp(code, "internal") == 0 ? KB_EXIT_FATAL : KB_EXIT_ERR;
        return false;
    }
    return true;
}

/* ---- kb add --batch ------------------------------------------------------
 *
 * §2's POST /documents/batch: "many at once, one transaction". One JSON
 * object a line on stdin — title, collection and content, and url, mime and
 * meta when there are any — filed under one lock with one index rebuild.
 *
 * ALL OR NOTHING, as far as the caller can cause it: every line is parsed and
 * prepared before the store is opened, so a bad line refuses the whole batch
 * with its line number and nothing is written. What can still fail midway is
 * the disk itself, and that is reported as `internal` like any other write.
 */
static int32_t add_batch(Arena *a, bool json) {
    char *text;
    size_t len;
    if (!read_text_arg(a, "-", &text, &len)) {
        err_out(json, "usage", "cannot read the batch from stdin");
        return KB_EXIT_ERR;
    }
    Filing *v = NULL;
    size_t n = 0, cap = 0;
    char err[512];
    int32_t lineno = 0;
    for (char *p = text; p < text + len;) {
        char *nl = memchr(p, '\n', (size_t)(text + len - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(text + len - p);
        lineno++;
        char *line = p;
        p = nl ? nl + 1 : text + len;
        while (ll && (line[ll - 1] == '\r' || line[ll - 1] == ' '))
            ll--;
        if (ll == 0)
            continue;
        char jerr[256];
        JVal *j = json_parse(a, line, ll, jerr, sizeof jerr);
        if (!j || j->t != J_OBJ) {
            err_out(json, "usage", "batch line %d is not a JSON object%s%s",
                    lineno, j ? "" : ": ", j ? "" : jerr);
            return KB_EXIT_ERR;
        }
        ARENA_GROW(a, v, n, cap, Filing);
        Filing *f = &v[n];
        memset(f, 0, sizeof *f);
        f->title = jobj_str(j, "title");
        f->collection = jobj_str(j, "collection");
        f->url = jobj_str(j, "url");
        f->mime = jobj_str(j, "mime");
        JVal *c = jobj_get(j, "content");
        if (c && c->t == J_STR) {
            f->content = c->s.ptr;
            f->len = c->s.len;
        }
        JVal *m = jobj_get(j, "meta");
        if (m && m->t != J_OBJ && m->t != J_NULL) {
            err_out(json, "usage", "batch line %d: meta must be a JSON object",
                    lineno);
            return KB_EXIT_ERR;
        }
        if (m && m->t == J_OBJ)
            f->meta = arena_strndup(a, m->src.ptr, m->src.len);
        const char *code;
        if (!prepare(a, f, &code, err, sizeof err)) {
            err_out(json, code, "batch line %d: %s", lineno, err);
            return KB_EXIT_ERR;
        }
        n++;
    }
    if (n == 0) {
        err_out(json, "usage", "an empty batch: pass one JSON object a line "
                               "on stdin");
        return KB_EXIT_ERR;
    }

    Store s;
    int32_t rc = KB_EXIT_OK;
    if (!open_store(a, &s, json, &rc))
        return rc;
    char now[32];
    plat_timestamp(now); /* one instant for the whole batch */
    Filed *r = (Filed *)arena_alloc0(a, n * sizeof(Filed));
    bool any_reindexed = false;
    for (size_t i = 0; i < n; i++) {
        if (!ingest(a, &s, &v[i], now, &r[i], err, sizeof err) ||
            !reread(a, &s, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
        any_reindexed |= r[i].reindexed;
    }
    if (any_reindexed && !rebuild(a, &s, err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,\"added\":[");
        for (size_t i = 0; i < n; i++) {
            sb_puts(&sb, i ? ",{" : "{");
            filed_json(&sb, &v[i], &r[i], now);
            sb_putc(&sb, '}');
        }
        sb_printf(&sb, "],\"count\":%zu}", n);
        puts(sb_finish(&sb));
    } else {
        for (size_t i = 0; i < n; i++)
            filed_human(a, &v[i], &r[i]);
    }
    store_close(&s);
    return KB_EXIT_OK;
}

/* ---- kb add ------------------------------------------------------------- */

int32_t cmd_add(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    if (has_flag(argc, argv, VALUE_FLAGS, "--batch")) {
        /* Every document says its own title, collection and the rest, so a
         * flag for one of them would say it for none. */
        for (int32_t i = 0; VALUE_FLAGS[i]; i++) {
            if (flag_value(argc, argv, VALUE_FLAGS, VALUE_FLAGS[i])) {
                err_out(json, "usage", "--batch takes each document's fields "
                                       "from its own line, not %s",
                        VALUE_FLAGS[i]);
                return KB_EXIT_ERR;
            }
        }
        return add_batch(a, json);
    }
    Filing f;
    memset(&f, 0, sizeof f);
    f.title = flag_value(argc, argv, VALUE_FLAGS, "--title");
    f.collection = flag_value(argc, argv, VALUE_FLAGS, "--collection");
    f.url = flag_value(argc, argv, VALUE_FLAGS, "--url");
    f.file = flag_value(argc, argv, VALUE_FLAGS, "--file");
    f.mime = flag_value(argc, argv, VALUE_FLAGS, "--mime");
    if (!f.title || !f.title[0]) {
        err_out(json, "usage", "kb add requires --title");
        return KB_EXIT_ERR;
    }
    if (!f.collection || !f.collection[0]) {
        err_out(json, "usage", "kb add requires --collection");
        return KB_EXIT_ERR;
    }
    char err[512];
    if (!meta_arg(a, argc, argv, &f.meta, err, sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    char *content;
    if (f.file) {
        if (!read_text_arg(a, f.file, &content, &f.len)) {
            err_out(json, "not_found", "cannot read %s", f.file);
            return KB_EXIT_ERR;
        }
    } else if (!read_text_arg(a, "-", &content, &f.len)) {
        err_out(json, "usage", "cannot read content from stdin");
        return KB_EXIT_ERR;
    }
    f.content = content;
    const char *code;
    if (!prepare(a, &f, &code, err, sizeof err)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    int32_t rc = KB_EXIT_OK;
    if (!open_store(a, &s, json, &rc))
        return rc;
    char now[32];
    plat_timestamp(now);
    Filed r;
    if (!ingest(a, &s, &f, now, &r, err, sizeof err) ||
        (r.reindexed &&
         (!reread(a, &s, err, sizeof err) || !rebuild(a, &s, err, sizeof err)))) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,");
        filed_json(&sb, &f, &r, now);
        sb_putc(&sb, '}');
        puts(sb_finish(&sb));
    } else {
        filed_human(a, &f, &r);
    }
    store_close(&s);
    return KB_EXIT_OK;
}
