#include "cmd.h"
#include "dirscan.h"
#include "modelrec.h"
#include "vectors.h"

#include "sha256.h"

#include <ctype.h>

/* POST /documents (§2): the caller already has the text and hands it over
 * rather than causing a second fetch. This is the load-bearing route — an
 * agent files what it just read as it goes, so the second question on the
 * same topic is answered from disk.
 */

static const char *const VALUE_FLAGS[] = {
    "--title", "--collection", "--url",  "--mime", "--etag",
    "--file",  "--meta",       "--meta-file", "--dir", "--embed-budget", NULL};
static const char *const BOOL_FLAGS[] = {"--json", "--batch", "--no-forget",
                                         "--wait", NULL};

/* --embed-budget S: seconds to spend embedding before answering (0: none
 * now); --wait: as long as it takes. What is left is pending for `kb embed`
 * and searchable by keyword at once. */
static bool budget_arg(int32_t argc, char **argv, bool json, double *out) {
    *out = KB_EMBED_BUDGET_S;
    const char *s = flag_value(argc, argv, VALUE_FLAGS, "--embed-budget");
    bool wait = has_flag(argc, argv, VALUE_FLAGS, "--wait");
    if (s && wait) {
        err_out(json, "usage", "--embed-budget and --wait are alternatives");
        return false;
    }
    if (wait)
        *out = VEC_NO_BUDGET;
    if (s) {
        char *end;
        double v = strtod(s, &end);
        if (*s == '\0' || *end != '\0' || v < 0 || v > 86400) {
            err_out(json, "usage", "--embed-budget takes a number of seconds, not \"%s\"", s);
            return false;
        }
        *out = v;
    }
    return true;
}

/* The type of text that came with no name to read it from — piped, or handed
 * over in a batch. Only the two structured types are recognised, and only on
 * evidence a plain text file would not have by accident: an HTML document
 * that says so on its first line, or Markdown that opens with a heading or
 * has at least two section headings below the top level. A `# comment` line
 * in a config file is one heading and not the first line, so it stays plain
 * text; being wrong the other way costs no more than the sliding window. */
static size_t atx_level(const char *line, size_t len) {
    size_t n = 0;
    while (n < len && n < 7 && line[n] == '#')
        n++;
    return (n >= 1 && n <= 6 && n < len && line[n] == ' ') ? n : 0;
}

static bool starts_icase(const char *s, size_t len, const char *prefix) {
    size_t n = strlen(prefix);
    if (len < n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != prefix[i])
            return false;
    return true;
}

static const char *mime_from_content(const char *text, size_t len) {
    size_t i = 0;
    if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0)
        i = 3;
    while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                       text[i] == '\n'))
        i++;
    if (starts_icase(text + i, len - i, "<!doctype html") ||
        starts_icase(text + i, len - i, "<html"))
        return "text/html";
    bool first = true;
    uint32_t sections = 0;
    for (size_t p = i; p < len;) {
        const char *nl = memchr(text + p, '\n', len - p);
        size_t end = nl ? (size_t)(nl - text) : len;
        size_t level = atx_level(text + p, end - p);
        if (first && level > 0)
            return "text/markdown";
        if (level >= 2 && ++sections >= 2)
            return "text/markdown";
        first = false;
        p = end + 1;
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
    const char *etag; /* the ETag the caller's fetch saw, or NULL */
    /* A document within its source: a file's path under a filed folder.
     * NULL is the source's one document (""). */
    const char *path;
    const char *source_title; /* NULL: the document's title */
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
    if (f->url && f->url[0] && chunk_mime_from_path(f->url))
        path_hint = f->url;
    else if (have_file && chunk_mime_from_path(f->file))
        path_hint = f->file;
    else
        path_hint = (f->url && f->url[0]) ? f->url : f->file;
    if (!f->mime)
        f->mime = chunk_mime_from_path(path_hint);
    if (!f->mime)
        f->mime = mime_from_content(f->content, f->len);
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
    const char *path = f->path ? f->path : "";
    const Document *existing_doc =
        existing_src ? doc_by_source_path(&s->documents, existing_src->id, path)
                     : NULL;
    out->blob_written = false;
    if (existing_doc) {
        /* The same locator again: a touch when the text is unchanged, a new
         * version under the same id when it is not. */
        if (!refile_document(a, s, existing_doc, f->content, f->len, f->hash,
                             f->title, f->mime, f->meta, f->etag, now,
                             &out->d, &out->reindexed, &out->blob_written,
                             err, errsz) ||
            !source_set_status(a, s, existing_src, "ok", err, errsz))
            return false;
        out->lang = doc_lang(out->d.mime, out->d.path);
        out->created = false;
        return true;
    }
    if (!store_put_blob(s, f->content, f->len, (char *)f->hash,
                        &out->blob_written, err, errsz))
        return false;
    /* Derived from what the record will carry (its mime, and its path within
     * a filed folder), never from the path hint. The hint is how the mime
     * was guessed; it is not stored, and a reader that
     * only has the record has to reach the same splitter or the chunk range
     * in that record stops describing the chunks on disk (see doc_lang). */
    out->lang = doc_lang(f->mime, path);
    ChunkParams cp = store_chunk_params(a, s);
    Chunks chunks = chunk_split(a, f->content, f->len, out->lang, doc_syntax(f->mime, path),
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
        if (!source_set_status(a, s, existing_src, "ok", err, errsz))
            return false;
    } else {
        Source src;
        memset(&src, 0, sizeof src);
        src.id = kb_id_make(a, 'S', src_n);
        src.kind = f->kind;
        src.locator = f->locator;
        src.title = f->source_title ? f->source_title : f->title;
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
    d->path = path;
    d->title = f->title;
    d->mime = f->mime;
    d->content_hash = arena_strdup(a, f->hash);
    d->bytes = (uint64_t)f->len;
    d->fetched_at = now;
    d->indexed_at = now;
    d->meta = f->meta;
    d->etag = f->etag;
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

/* The line a person gets when embedding was left for later. */
static void pending_note(size_t pending) {
    if (pending)
        printf("%zu chunk%s left to embed: searchable by keyword now; kb embed finishes them\n",
               pending, pending == 1 ? "" : "s");
}

/* ---- kb add --batch ------------------------------------------------------
 *
 * §2's POST /documents/batch: "many at once, one transaction". One JSON
 * object a line on stdin — title, collection and content, and url, mime,
 * meta and etag when there are any — filed under one lock with one index rebuild.
 *
 * ALL OR NOTHING, as far as the caller can cause it: every line is parsed and
 * prepared before the store is opened, so a bad line refuses the whole batch
 * with its line number and nothing is written. What can still fail midway is
 * the disk itself, and that is reported as `internal` like any other write.
 */
static int32_t add_batch(Arena *a, bool json, double budget_s) {
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
        f->etag = jobj_str(j, "etag");
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
    /* §8: the model is recorded at first ingest, when there is one, and the
     * new chunks are embedded under it. */
    VecSync vs;
    bool embedded;
    if ((any_reindexed && !rebuild(a, &s, err, sizeof err)) ||
        !model_record_if_absent(a, &s, err, sizeof err) ||
        !vec_update(a, &s, false, false, budget_s, &vs, &embedded, err, sizeof err)) {
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
        sb_printf(&sb, "],\"count\":%zu,\"pending\":%zu}", n, vs.pending);
        puts(sb_finish(&sb));
    } else {
        for (size_t i = 0; i < n; i++)
            filed_human(a, &v[i], &r[i]);
        pending_note(vs.pending);
    }
    store_close(&s);
    return KB_EXIT_OK;
}

/* ---- kb add --dir ------------------------------------------------------
 *
 * A folder, as its repository sees it (dirscan.h says which files), filed as
 * ONE `dir` source whose locator is the folder's absolute path, with one
 * document per file at its path under the folder. Filing the same folder into
 * the same collection again finds that source and each document by path, so
 * it is incremental without any state of its own:
 *
 *   - a file whose text is unchanged is touched (§2), not re-indexed;
 *   - a changed file is a new version of its document, same id;
 *   - a new file is a new document;
 *   - a file that is gone — deleted, or now ignored — is forgotten, unless
 *     `forget` is off (the MCP tool: forgetting is not an agent's call, §9),
 *     and then it is reported as missing instead.
 *
 * One lock, one keyword rebuild and one embedding pass for the whole folder,
 * as a batch has.
 */

/* A code file's meta (§1.2): its language and the definitions in it, name,
 * kind and line, the first KB_META_SYMBOLS of them. NULL for anything else. */
#define KB_META_SYMBOLS 500

static const char *code_meta(Arena *a, const char *path, const char *mime,
                             const char *text, size_t len) {
    SyntaxLang l = doc_syntax(mime, path);
    SyntaxSymbol *syms;
    size_t n;
    if (l == SYNTAX_NONE || !syntax_symbols(a, l, text, len, &syms, &n))
        return NULL;
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"language\":\"%s\",\"symbols\":[", syntax_lang_name(l));
    for (size_t i = 0; i < n && i < KB_META_SYMBOLS; i++) {
        sb_puts(&sb, i ? ",{\"name\":" : "{\"name\":");
        json_escape_c(&sb, syms[i].name);
        sb_puts(&sb, ",\"kind\":");
        json_escape_c(&sb, syms[i].kind);
        sb_printf(&sb, ",\"line\":%u}", syms[i].line);
    }
    sb_puts(&sb, "]}");
    return sb_finish(&sb);
}

static int32_t cmp_str(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

int32_t add_dir(Arena *a, bool json, const char *dir, const char *collection,
                bool forget, double budget_s) {
    char err[512];
    char root[KB_PATH_MAX];
    if (!plat_realpath(dir, root, sizeof root) || !plat_is_dir(root)) {
        err_out(json, "not_found", "%s is not a directory", dir);
        return KB_EXIT_ERR;
    }
    if (!collection || !collection[0] || strchr(collection, '/') ||
        strchr(collection, '\\')) {
        err_out(json, "usage", collection && collection[0]
                                   ? "collection names do not nest"
                                   : "kb add --dir requires --collection");
        return KB_EXIT_ERR;
    }
    const bool tty = plat_stderr_tty();
    DirScan scan;
    if (tty)
        fprintf(stderr, "scanning %s\n", root);
    if (!dir_scan(a, root, &scan, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }
    const char *base = strrchr(root, '/');
    base = base && base[1] ? base + 1 : root;

    Store s;
    int32_t rc = KB_EXIT_OK;
    if (!open_store(a, &s, json, &rc))
        return rc;
    char now[32];
    plat_timestamp(now);
    size_t added = 0, updated = 0, unchanged = 0;
    const char *source_id = NULL;
    for (size_t i = 0; i < scan.n; i++) {
        const DirFile *df = &scan.v[i];
        Filing f;
        memset(&f, 0, sizeof f);
        f.title = arena_printf(a, "%s/%s", base, df->rel);
        f.source_title = base;
        f.collection = collection;
        f.path = df->rel;
        f.mime = df->mime;
        f.content = df->content;
        f.len = df->len;
        f.kind = "dir";
        f.locator = root;
        f.meta = code_meta(a, df->rel, df->mime, df->content, df->len);
        sha256_hex(f.content, f.len, f.hash);
        Filed r;
        /* The first file creates the source; every later one has to find
         * it, so the logs are read again once, then. Documents are looked
         * up by path only for files seen before, which are in the fold. */
        const bool had_source = source_id != NULL;
        if (!ingest(a, &s, &f, now, &r, err, sizeof err) ||
            (!had_source && !reread(a, &s, err, sizeof err))) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
        source_id = r.d.source;
        if (r.created)
            added++;
        else if (r.reindexed)
            updated++;
        else
            unchanged++;
        if (tty)
            fprintf(stderr, "\rfiling %zu/%zu files", i + 1, scan.n);
    }
    if (tty && scan.n)
        fputc('\n', stderr);
    if (!reread(a, &s, err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    if (!source_id) {
        const Source *src = src_by_key(&s.sources, "dir", root, collection);
        source_id = src ? src->id : NULL;
    }

    /* What the source holds that the folder no longer does. */
    const char **seen = (const char **)arena_alloc(
        a, (scan.n ? scan.n : 1) * sizeof(char *));
    for (size_t i = 0; i < scan.n; i++)
        seen[i] = scan.v[i].rel;
    qsort(seen, scan.n, sizeof(char *), cmp_str);
    const char **gone_ids = (const char **)arena_alloc(
        a, (s.documents.n ? s.documents.n : 1) * sizeof(char *));
    const char **gone_paths = (const char **)arena_alloc(
        a, (s.documents.n ? s.documents.n : 1) * sizeof(char *));
    size_t ngone = 0;
    for (size_t i = 0; source_id && i < s.documents.n; i++) {
        const Document *d = &s.documents.v[i];
        if (strcmp(d->source, source_id) != 0)
            continue;
        if (bsearch(&d->path, seen, scan.n, sizeof(char *), cmp_str))
            continue;
        gone_ids[ngone] = d->id;
        gone_paths[ngone++] = d->path;
    }
    if (forget && ngone &&
        (!forget_records(a, &s, gone_ids, ngone, NULL, 0, err, sizeof err) ||
         !reread(a, &s, err, sizeof err))) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }

    VecSync vs;
    memset(&vs, 0, sizeof vs);
    bool embedded = false;
    bool changed = added || updated || (forget && ngone);
    if ((changed && !rebuild(a, &s, err, sizeof err)) ||
        !model_record_if_absent(a, &s, err, sizeof err) ||
        (changed &&
         !vec_update(a, &s, false, tty, budget_s, &vs, &embedded, err, sizeof err))) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"source\":");
        if (source_id)
            json_escape_c(&sb, source_id);
        else
            sb_puts(&sb, "null");
        sb_puts(&sb, ",\"root\":");
        json_escape_c(&sb, root);
        sb_puts(&sb, ",\"collection\":");
        json_escape_c(&sb, collection);
        sb_printf(&sb,
                  ",\"files\":%zu,\"added\":%zu,\"updated\":%zu,"
                  "\"unchanged\":%zu,\"forgotten\":[",
                  scan.n, added, updated, unchanged);
        for (size_t i = 0; forget && i < ngone; i++) {
            sb_puts(&sb, i ? "," : "");
            json_escape_c(&sb, gone_ids[i]);
        }
        sb_puts(&sb, "],\"missing\":[");
        for (size_t i = 0; !forget && i < ngone; i++) {
            sb_puts(&sb, i ? "," : "");
            json_escape_c(&sb, gone_paths[i]);
        }
        sb_printf(&sb,
                  "],\"skipped\":{\"ignored\":%zu,\"hidden\":%zu,"
                  "\"vendored\":%zu,\"generated\":%zu,\"binary\":%zu,"
                  "\"large\":%zu,\"unreadable\":%zu,\"otherTypes\":%zu},"
                  "\"embedded\":%zu,\"pending\":%zu}",
                  scan.ignored, scan.hidden, scan.vendored, scan.generated,
                  scan.binary, scan.large, scan.unreadable, scan.other,
                  vs.embedded, vs.pending);
    } else {
        sb_printf(&sb, "%s  ", source_id ? source_id : "-");
        sb_puts_safe(&sb, collection);
        sb_printf(&sb, "  %s: %zu file%s, %zu new, %zu updated, %zu unchanged",
                  base, scan.n, scan.n == 1 ? "" : "s", added, updated,
                  unchanged);
        if (ngone)
            sb_printf(&sb, ", %zu %s", ngone, forget ? "forgotten" : "missing");
        sb_printf(&sb, "\nskipped: %zu ignored by .gitignore, %zu hidden, "
                       "%zu vendored, %zu generated, %zu binary or empty, "
                       "%zu too large, %zu other types",
                  scan.ignored, scan.hidden, scan.vendored, scan.generated,
                  scan.binary, scan.large, scan.other);
        if (scan.unreadable)
            sb_printf(&sb, ", %zu unreadable", scan.unreadable);
        if (vs.embedded)
            sb_printf(&sb, "\nembedded %zu chunk%s", vs.embedded,
                      vs.embedded == 1 ? "" : "s");
        for (size_t i = 0; !forget && i < ngone; i++)
            sb_printf(&sb, "\nmissing: %s", gone_paths[i]);
        if (vs.pending)
            sb_printf(&sb, "\n%zu chunk%s left to embed: searchable by keyword now; "
                           "kb embed finishes them",
                      vs.pending, vs.pending == 1 ? "" : "s");
        sb_putc(&sb, '\n');
    }
    fputs(sb_finish(&sb), stdout);
    if (json)
        fputc('\n', stdout);
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
            if (strcmp(VALUE_FLAGS[i], "--embed-budget") == 0)
                continue; /* how long to embed is the batch's, not a field */
            if (flag_value(argc, argv, VALUE_FLAGS, VALUE_FLAGS[i])) {
                err_out(json, "usage", "--batch takes each document's fields "
                                       "from its own line, not %s",
                        VALUE_FLAGS[i]);
                return KB_EXIT_ERR;
            }
        }
        double budget;
        if (!budget_arg(argc, argv, json, &budget))
            return KB_EXIT_ERR;
        return add_batch(a, json, budget);
    }
    const char *dir = flag_value(argc, argv, VALUE_FLAGS, "--dir");
    if (dir) {
        /* Each file is its own document with its own name and type; only
         * the collection is the folder's. */
        static const char *const per_file[] = {"--title", "--url", "--mime",
                                               "--etag", "--file", "--meta",
                                               "--meta-file", NULL};
        for (int32_t i = 0; per_file[i]; i++) {
            if (flag_value(argc, argv, VALUE_FLAGS, per_file[i])) {
                err_out(json, "usage", "--dir files each file under its own "
                                       "path and type, not %s",
                        per_file[i]);
                return KB_EXIT_ERR;
            }
        }
        double budget;
        if (!budget_arg(argc, argv, json, &budget))
            return KB_EXIT_ERR;
        return add_dir(a, json, dir,
                       flag_value(argc, argv, VALUE_FLAGS, "--collection"),
                       !has_flag(argc, argv, VALUE_FLAGS, "--no-forget"), budget);
    }
    if (has_flag(argc, argv, VALUE_FLAGS, "--no-forget")) {
        err_out(json, "usage", "--no-forget goes with --dir");
        return KB_EXIT_ERR;
    }
    Filing f;
    memset(&f, 0, sizeof f);
    f.title = flag_value(argc, argv, VALUE_FLAGS, "--title");
    f.collection = flag_value(argc, argv, VALUE_FLAGS, "--collection");
    f.url = flag_value(argc, argv, VALUE_FLAGS, "--url");
    f.file = flag_value(argc, argv, VALUE_FLAGS, "--file");
    f.mime = flag_value(argc, argv, VALUE_FLAGS, "--mime");
    f.etag = flag_value(argc, argv, VALUE_FLAGS, "--etag");
    double budget;
    if (!budget_arg(argc, argv, json, &budget))
        return KB_EXIT_ERR;
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
    VecSync vs;
    memset(&vs, 0, sizeof vs);
    bool embedded;
    if (!ingest(a, &s, &f, now, &r, err, sizeof err) ||
        (r.reindexed &&
         (!reread(a, &s, err, sizeof err) || !rebuild(a, &s, err, sizeof err))) ||
        !model_record_if_absent(a, &s, err, sizeof err) ||
        (r.reindexed &&
         !vec_update(a, &s, false, false, budget, &vs, &embedded, err, sizeof err))) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":true,");
        filed_json(&sb, &f, &r, now);
        sb_printf(&sb, ",\"pending\":%zu}", vs.pending);
        puts(sb_finish(&sb));
    } else {
        filed_human(a, &f, &r);
        pending_note(vs.pending);
    }
    store_close(&s);
    return KB_EXIT_OK;
}
