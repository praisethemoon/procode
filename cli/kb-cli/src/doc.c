#include "doc.h"

#include "platform.h"

/* ---- id helpers ------------------------------------------------------- */

/* "D-241" -> 241 for prefix 'D'; 0 for anything else (NULL, wrong kind,
 * missing hyphen, non-digits, trailing garbage). One parser, so every
 * caller agrees on what an identifier is. */
int64_t kb_id_num(const char *id, char prefix) {
    if (!id || id[0] != prefix || id[1] != '-' || !id[2])
        return 0;
    char *end;
    long long v = strtoll(id + 2, &end, 10);
    return (*end == '\0' && v > 0) ? (int64_t)v : 0;
}

char *kb_id_make(Arena *a, char prefix, int64_t n) {
    return arena_printf(a, "%c-%lld", prefix, (long long)n);
}

/* ---- encoding --------------------------------------------------------- */

static void put_str(StrBuf *sb, const char *key, const char *val) {
    sb_printf(sb, ",\"%s\":", key);
    if (val)
        json_escape_c(sb, val);
    else
        sb_puts(sb, "null");
}

char *doc_encode_source(Arena *a, const Source *s, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"type\":\"source\",\"id\":\"%s\"", s->id);
    put_str(&sb, "kind", s->kind);
    put_str(&sb, "locator", s->locator);
    put_str(&sb, "title", s->title);
    put_str(&sb, "collection", s->collection);
    put_str(&sb, "createdAt", s->created_at);
    sb_putc(&sb, '}');
    *out_len = sb.len;
    return sb_finish(&sb);
}

char *doc_encode_document(Arena *a, const Document *d, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"type\":\"document\",\"id\":\"%s\",\"source\":\"%s\"",
              d->id, d->source);
    put_str(&sb, "path", d->path);
    put_str(&sb, "title", d->title);
    put_str(&sb, "mime", d->mime);
    put_str(&sb, "contentHash", d->content_hash);
    sb_printf(&sb, ",\"bytes\":%llu", (unsigned long long)d->bytes);
    put_str(&sb, "fetchedAt", d->fetched_at);
    put_str(&sb, "indexedAt", d->indexed_at);
    sb_printf(&sb, ",\"chunkCount\":%lu,\"chunkBase\":%lld",
              (unsigned long)d->chunk_count, (long long)d->chunk_base);
    if (d->meta) {
        /* the writer's bytes, unchanged: what was handed in is what comes
         * back out, not what a printer made of it */
        sb_puts(&sb, ",\"meta\":");
        sb_puts(&sb, d->meta);
    }
    sb_putc(&sb, '}');
    *out_len = sb.len;
    return sb_finish(&sb);
}

char *doc_encode_touch(Arena *a, const char *id, const char *fetched_at,
                       size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"type\":\"touch\",\"id\":\"%s\"", id);
    put_str(&sb, "fetchedAt", fetched_at);
    sb_putc(&sb, '}');
    *out_len = sb.len;
    return sb_finish(&sb);
}

const char *const LINK_TYPES[] = {"supersedes", "cites", "analogue_of",
                                  "implements", "see_also", NULL};

const char *link_type_canon(const char *rel) {
    if (!rel)
        return NULL;
    for (int32_t i = 0; LINK_TYPES[i]; i++) {
        if (strcmp(rel, LINK_TYPES[i]) == 0)
            return LINK_TYPES[i];
    }
    return NULL;
}

/* from/to/rel go in unescaped, like every other id in this file: they are
 * `D-<n>` and a pointer into LINK_TYPES by the time a writer gets here, and
 * neither spelling can contain a byte JSON would need to escape. */
char *doc_encode_link(Arena *a, const Link *l, bool present, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"type\":\"%s\",\"from\":\"%s\",\"rel\":\"%s\","
                   "\"to\":\"%s\"",
              present ? "link" : "unlink", l->from, l->rel, l->to);
    put_str(&sb, "createdAt", l->created_at);
    sb_putc(&sb, '}');
    *out_len = sb.len;
    return sb_finish(&sb);
}

char *doc_encode_source_forget(Arena *a, const char *id, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"type\":\"forget\",\"id\":\"%s\"}", id);
    *out_len = sb.len;
    return sb_finish(&sb);
}

/* The same record shape as a source's: the log it is in says which kind of
 * id it removes. */
char *doc_encode_document_forget(Arena *a, const char *id, size_t *out_len) {
    return doc_encode_source_forget(a, id, out_len);
}

/* ---- loading ---------------------------------------------------------- */

/* Splits a log file into complete lines. A crash mid-append leaves an
 * unterminated final line: nobody ever acknowledged that record, so it is
 * dropped here and truncated away by the next writer (store_repair). */
static bool log_lines(Arena *a, const char *path, Lines *out, bool *torn,
                      char *err, size_t errsz) {
    *torn = false;
    char *data;
    size_t len;
    /* No size cap: kb's own logs must never become unreadable by growing. */
    if (!plat_read_file_max(a, path, &data, &len, (size_t)-1)) {
        if (plat_is_file(path)) {
            snprintf(err, errsz, "cannot read %s", path);
            return false;
        }
        out->lines = NULL; /* no log yet is an empty log */
        out->count = 0;
        out->eof_nl = true;
        return true;
    }
    if (len > 0 && data[len - 1] != '\n') {
        size_t cut = len;
        while (cut > 0 && data[cut - 1] != '\n')
            cut--;
        *torn = true;
        len = cut;
    }
    *out = split_lines(a, data, len);
    return true;
}

static JVal *parse_record(Arena *a, Str line, int32_t lineno, const char *path,
                          char *err, size_t errsz) {
    char jerr[256];
    JVal *v = json_parse(a, line.ptr, line.len, jerr, sizeof jerr);
    if (!v || v->t != J_OBJ) {
        snprintf(err, errsz, "%s line %d: %s", path, lineno,
                 v ? "record is not a JSON object" : jerr);
        return NULL;
    }
    return v;
}

bool srclog_load(Arena *a, const char *path, SourceList *out, char *err,
                 size_t errsz) {
    memset(out, 0, sizeof(*out));
    Lines l;
    if (!log_lines(a, path, &l, &out->torn_tail, err, errsz))
        return false;
    Source *v = (Source *)arena_alloc(
        a, (size_t)(l.count ? l.count : 1) * sizeof(Source));
    size_t n = 0;
    for (int32_t i = 0; i < l.count; i++) {
        if (l.lines[i].len == 0)
            continue;
        JVal *j = parse_record(a, l.lines[i], i + 1, path, err, errsz);
        if (!j)
            return false;
        const char *id = jobj_str(j, "id");
        int64_t num = kb_id_num(id, 'S');
        if (num > out->max_id)
            out->max_id = num;
        const char *type = jobj_str(j, "type");
        if (type && strcmp(type, "forget") == 0) {
            /* §7's DELETE /collections/{name}, the only writer of these. The
             * id stays floored into max_id above, so forgetting a source
             * never lets its identifier come back (§1.1). */
            for (size_t k = 0; k < n; k++) {
                if (id && strcmp(v[k].id, id) == 0) {
                    memmove(&v[k], &v[k + 1], (n - k - 1) * sizeof(Source));
                    n--;
                    break;
                }
            }
            continue;
        }
        if (!type || strcmp(type, "source") != 0)
            continue; /* a record kind this build does not know: skip it */
        Source s;
        memset(&s, 0, sizeof s);
        s.id = id;
        s.kind = jobj_str(j, "kind");
        s.locator = jobj_str(j, "locator");
        s.title = jobj_str(j, "title");
        s.collection = jobj_str(j, "collection");
        s.created_at = jobj_str(j, "createdAt");
        if (!s.id || !s.kind || !s.locator || !s.collection) {
            snprintf(err, errsz, "%s line %d: source record missing a field",
                     path, i + 1);
            return false;
        }
        size_t at = n;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(v[k].id, s.id) == 0) {
                at = k;
                break;
            }
        }
        v[at] = s;
        if (at == n)
            n++;
    }
    out->v = v;
    out->n = n;
    return true;
}

bool doclog_load(Arena *a, const char *path, DocList *out, char *err,
                 size_t errsz) {
    memset(out, 0, sizeof(*out));
    Lines l;
    if (!log_lines(a, path, &l, &out->torn_tail, err, errsz))
        return false;
    Document *v = (Document *)arena_alloc(
        a, (size_t)(l.count ? l.count : 1) * sizeof(Document));
    size_t n = 0;
    Link *lk = NULL;
    size_t nlk = 0, lk_cap = 0;
    for (int32_t i = 0; i < l.count; i++) {
        if (l.lines[i].len == 0)
            continue;
        JVal *j = parse_record(a, l.lines[i], i + 1, path, err, errsz);
        if (!j)
            return false;
        const char *id = jobj_str(j, "id");
        int64_t num = kb_id_num(id, 'D');
        if (num > out->max_id)
            out->max_id = num;
        int64_t base = jobj_int(j, "chunkBase", 0);
        int64_t count = jobj_int(j, "chunkCount", 0);
        if (base > 0 && count > 0 && base + count - 1 > out->max_chunk_id)
            out->max_chunk_id = base + count - 1;
        const char *type = jobj_str(j, "type");
        if (!type) {
            snprintf(err, errsz, "%s line %d: record missing \"type\"", path,
                     i + 1);
            return false;
        }
        if (strcmp(type, "forget") == 0) {
            /* §2's DELETE /documents/{id}. The id and chunk range were
             * floored into max_id and max_chunk_id above, so neither can be
             * handed out again. A later `document` record under the same id
             * cannot happen: the counters never go back. */
            for (size_t k = 0; k < n; k++) {
                if (id && strcmp(v[k].id, id) == 0) {
                    memmove(&v[k], &v[k + 1], (n - k - 1) * sizeof(Document));
                    n--;
                    break;
                }
            }
            continue;
        }
        if (strcmp(type, "touch") == 0) {
            const char *at = jobj_str(j, "fetchedAt");
            for (size_t k = 0; k < n; k++) {
                if (id && strcmp(v[k].id, id) == 0 && at) {
                    v[k].fetched_at = at;
                    break;
                }
            }
            continue;
        }
        bool is_link = strcmp(type, "link") == 0;
        if (is_link || strcmp(type, "unlink") == 0) {
            /* §6, folded last-wins over the (from, rel, to) triple. A record
             * missing any of the three, or naming a type outside §6's five,
             * is a record this build cannot mean anything by; it is dropped
             * rather than half-applied. */
            Link e;
            e.from = jobj_str(j, "from");
            e.rel = link_type_canon(jobj_str(j, "rel"));
            e.to = jobj_str(j, "to");
            e.created_at = jobj_str(j, "createdAt");
            if (!e.from || !e.rel || !e.to)
                continue;
            size_t at = nlk;
            for (size_t k = 0; k < nlk; k++) {
                /* Compared as text, not by pointer. Both sides do come from
                 * LINK_TYPES today, but link_find next door compares them as
                 * text and one rule for "the same edge" is worth more than
                 * one saved comparison. */
                if (strcmp(lk[k].from, e.from) == 0 &&
                    strcmp(lk[k].rel, e.rel) == 0 &&
                    strcmp(lk[k].to, e.to) == 0) {
                    at = k;
                    break;
                }
            }
            if (!is_link) {
                if (at < nlk) {
                    memmove(&lk[at], &lk[at + 1], (nlk - at - 1) * sizeof(Link));
                    nlk--;
                }
                continue;
            }
            if (at < nlk) {
                lk[at] = e; /* a re-link refreshes createdAt, nothing else */
                continue;
            }
            ARENA_GROW(a, lk, nlk, lk_cap, Link);
            lk[nlk++] = e;
            continue;
        }
        if (strcmp(type, "document") != 0)
            continue; /* a record kind this build does not know: skip it */
        Document d;
        memset(&d, 0, sizeof d);
        d.id = id;
        d.source = jobj_str(j, "source");
        d.path = jobj_str(j, "path");
        d.title = jobj_str(j, "title");
        d.mime = jobj_str(j, "mime");
        d.content_hash = jobj_str(j, "contentHash");
        d.fetched_at = jobj_str(j, "fetchedAt");
        d.indexed_at = jobj_str(j, "indexedAt");
        d.bytes = (uint64_t)jobj_int(j, "bytes", 0);
        d.chunk_count = (uint32_t)count;
        d.chunk_base = base;
        JVal *m = jobj_get(j, "meta");
        if (m && m->t == J_OBJ && m->src.ptr)
            d.meta = arena_strndup(a, m->src.ptr, m->src.len);
        if (!d.id || !d.source || !d.path || !d.content_hash) {
            snprintf(err, errsz, "%s line %d: document record missing a field",
                     path, i + 1);
            return false;
        }
        size_t at = n;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(v[k].id, d.id) == 0) {
                at = k;
                break;
            }
        }
        v[at] = d;
        if (at == n)
            n++;
    }
    out->v = v;
    out->n = n;
    out->links = lk;
    out->nlinks = nlk;
    return true;
}

/* ---- lookups ---------------------------------------------------------- */

const Source *src_by_id(const SourceList *l, const char *id) {
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].id, id) == 0)
            return &l->v[i];
    }
    return NULL;
}

const Source *src_by_key(const SourceList *l, const char *kind,
                         const char *locator, const char *collection) {
    /* A source is identified by what it points at AND the topic it was filed
     * under: stores and collections are orthogonal (§1.4), and the same URL
     * filed under two collections is two deliberate filings, not a
     * collision. */
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].kind, kind) == 0 &&
            strcmp(l->v[i].locator, locator) == 0 &&
            strcmp(l->v[i].collection, collection) == 0)
            return &l->v[i];
    }
    return NULL;
}

const Document *doc_by_id(const DocList *l, const char *id) {
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].id, id) == 0)
            return &l->v[i];
    }
    return NULL;
}

const Document *doc_by_source_path(const DocList *l, const char *source,
                                   const char *path) {
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].source, source) == 0 &&
            strcmp(l->v[i].path, path) == 0)
            return &l->v[i];
    }
    return NULL;
}

const Link *link_find(const DocList *l, const char *from, const char *rel,
                      const char *to) {
    for (size_t i = 0; i < l->nlinks; i++) {
        if (strcmp(l->links[i].from, from) == 0 &&
            strcmp(l->links[i].rel, rel) == 0 &&
            strcmp(l->links[i].to, to) == 0)
            return &l->links[i];
    }
    return NULL;
}
