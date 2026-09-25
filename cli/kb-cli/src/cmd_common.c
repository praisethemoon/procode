#include "cmd.h"

#include "sha256.h"

#include <stdarg.h>

static bool is_value_flag(const char *const *value_flags, const char *arg) {
    for (int32_t f = 0; value_flags && value_flags[f]; f++) {
        if (strcmp(arg, value_flags[f]) == 0)
            return true;
    }
    return false;
}

/* "--flag=value" -> the value, or NULL when arg is not that flag in that
 * form. The route in §2 is a query string, so callers reach for '=' out of
 * habit; accepting both spellings costs one comparison. */
static const char *eq_form(const char *arg, const char *flag) {
    size_t n = strlen(flag);
    if (strncmp(arg, flag, n) == 0 && arg[n] == '=')
        return arg + n + 1;
    return NULL;
}

bool has_flag(int32_t argc, char **argv, const char *const *value_flags,
              const char *flag) {
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return false; /* flags end here */
        if (strcmp(argv[i], flag) == 0 || eq_form(argv[i], flag))
            return true;
        if (is_value_flag(value_flags, argv[i]))
            i++; /* never read a flag's value as a flag */
    }
    return false;
}

const char *flag_value(int32_t argc, char **argv,
                       const char *const *value_flags, const char *flag) {
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return NULL;
        if (strcmp(argv[i], flag) == 0)
            return i + 1 < argc ? argv[i + 1] : NULL;
        const char *eq = eq_form(argv[i], flag);
        if (eq)
            return eq;
        if (is_value_flag(value_flags, argv[i]))
            i++;
    }
    return NULL;
}

const char *positional_arg(int32_t argc, char **argv,
                           const char *const *value_flags, int32_t index) {
    int32_t seen = 0;
    bool flags_ended = false;
    for (int32_t i = 0; i < argc; i++) {
        if (!flags_ended) {
            if (strcmp(argv[i], "--") == 0) {
                flags_ended = true;
                continue;
            }
            if (argv[i][0] == '-') {
                if (is_value_flag(value_flags, argv[i]))
                    i++;
                continue;
            }
        }
        if (seen == index)
            return argv[i];
        seen++;
    }
    return NULL;
}

const char *unknown_flag(int32_t argc, char **argv,
                         const char *const *value_flags,
                         const char *const *bool_flags) {
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return NULL;
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            continue;
        if (is_value_flag(value_flags, argv[i])) {
            i++;
            continue;
        }
        bool known = false;
        for (int32_t f = 0; value_flags && value_flags[f] && !known; f++)
            known = eq_form(argv[i], value_flags[f]) != NULL;
        for (int32_t f = 0; bool_flags && bool_flags[f] && !known; f++)
            known = strcmp(argv[i], bool_flags[f]) == 0;
        if (!known)
            return argv[i];
    }
    return NULL;
}

void err_out(bool json_mode, const char *code, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (json_mode) {
        /* minimal escaping: our own messages contain no quotes/control chars,
         * but paths could; escape conservatively */
        fputs("{\"ok\":false,\"error\":\"", stdout);
        fputs(code, stdout);
        fputs("\",\"message\":\"", stdout);
        for (const char *p = msg; *p; p++) {
            if (*p == '"' || *p == '\\')
                putchar('\\');
            if ((unsigned char)*p < 0x20)
                putchar(' ');
            else
                putchar(*p);
        }
        fputs("\"}\n", stdout);
    } else {
        fputs("error: ", stderr);
        /* the message is half ours and half not — most of them name a path —
         * so control bytes are flattened before they reach a terminal */
        for (const char *p = msg; *p; p++)
            fputc((unsigned char)*p < 0x20 ? ' ' : *p, stderr);
        fputc('\n', stderr);
    }
}

void sb_puts_safe(StrBuf *sb, const char *s) {
    for (const char *p = s; *p; p++)
        sb_putc(sb, (unsigned char)*p < 0x20 ? ' ' : *p);
}

bool read_text_arg(Arena *a, const char *path, char **out, size_t *out_len) {
    if (strcmp(path, "-") != 0)
        return plat_read_file(a, path, out, out_len);
    StrBuf sb;
    sb_init(&sb, a);
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, stdin)) > 0) {
        sb_putn(&sb, buf, got);
        if (sb.len > KB_MAX_FILE_SIZE)
            return false;
    }
    *out_len = sb.len;
    *out = sb_finish(&sb);
    return true;
}

/* ---- staleness (§5) --------------------------------------------------- */

bool duration_parse(const char *s, int64_t *seconds) {
    if (!s || !s[0])
        return false;
    int64_t n = 0;
    const char *p = s;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (n > (INT64_MAX - (*p - '0')) / 10)
            return false; /* a threshold nobody meant */
        n = n * 10 + (*p - '0');
    }
    if (p == s || p[0] == '\0' || p[1] != '\0')
        return false; /* no digits, no unit, or more than one unit letter */
    int64_t mul;
    switch (*p) {
    case 's': mul = 1; break;
    case 'm': mul = 60; break;
    case 'h': mul = 3600; break;
    case 'd': mul = 86400; break;
    case 'w': mul = 604800; break;
    /* No month and no year on purpose: both are calendar quantities whose
     * length depends on when you start counting, and §5's question — "is
     * this older than X" — deserves an answer that does not. */
    default: return false;
    }
    if (n > INT64_MAX / mul)
        return false;
    *seconds = n * mul;
    return true;
}

const char *older_than_arg(int32_t argc, char **argv,
                           const char *const *value_flags) {
    const char *v = flag_value(argc, argv, value_flags, "--older-than");
    return v ? v : flag_value(argc, argv, value_flags, "--olderThan");
}

bool staleness_init(Staleness *st, const char *older_than, char *err,
                    size_t errsz) {
    memset(st, 0, sizeof(*st));
    st->spec = older_than && older_than[0] ? older_than : KB_STALE_DEFAULT;
    if (!duration_parse(st->spec, &st->seconds)) {
        snprintf(err, errsz,
                 "\"%s\" is not a duration; write a count and a unit, one of "
                 "s m h d w (for example 90d)",
                 st->spec);
        return false;
    }
    st->now = plat_now_epoch();
    st->cutoff = st->now - st->seconds;
    plat_time_format(st->cutoff, st->cutoff_iso);
    return true;
}

bool doc_stale(const Staleness *st, const Document *d) {
    int64_t at;
    if (!d->fetched_at || !plat_time_parse(d->fetched_at, &at))
        return true;
    /* Strictly older than the cutoff. A document fetched exactly on the
     * boundary is inside the window the caller asked for, not outside it. */
    return at < st->cutoff;
}

/* ---- the store -------------------------------------------------------- */

bool store_resolve(char *dir, size_t dirsz, char *err, size_t errsz) {
    if (store_find(dir, dirsz))
        return true;
    snprintf(err, errsz, "no kb store at or above the current directory "
                         "(run \"kb init\")");
    return false;
}

/* ---- stored documents -------------------------------------------------- */

Lang doc_lang(const char *mime, const char *path) {
    return chunk_lang(mime, path);
}

bool doc_chunks(Arena *a, Store *s, const Document *d, char **text,
                size_t *len, Chunks *out) {
    if (!store_get_blob(s, d->content_hash, text, len))
        return false;
    /* The recorded parameters, not this build's: index/model.json states
     * what the store was actually built with, and chunking with anything
     * else would describe chunks the store does not contain (§8). */
    ChunkParams cp = store_chunk_params(a, s);
    *out = chunk_split(a, *text, *len, doc_lang(d->mime, d->path),
                       (size_t)cp.chunk_tokens * KB_BYTES_PER_TOKEN,
                       (size_t)cp.chunk_overlap * KB_BYTES_PER_TOKEN);
    return true;
}

const Document *doc_by_chunk(const DocList *l, int64_t chunk_num,
                             uint32_t *ordinal) {
    for (size_t i = 0; i < l->n; i++) {
        const Document *d = &l->v[i];
        if (d->chunk_base <= 0 || d->chunk_count == 0)
            continue;
        if (chunk_num >= d->chunk_base &&
            chunk_num < d->chunk_base + (int64_t)d->chunk_count) {
            *ordinal = (uint32_t)(chunk_num - d->chunk_base);
            return d;
        }
    }
    return NULL;
}

/* ---- the keyword index ------------------------------------------------- */

bool index_rebuild(Arena *a, Store *s, uint32_t *docs, uint32_t *missing_blobs,
                   FtsBuildStats *stats, char *err, size_t errsz) {
    *docs = *missing_blobs = 0;
    memset(stats, 0, sizeof(*stats));
    ChunkParams cp = store_chunk_params(a, s);
    size_t n = s->documents.n;
    FtsDocInput *in =
        (FtsDocInput *)arena_alloc0(a, (n ? n : 1) * sizeof(FtsDocInput));
    for (size_t i = 0; i < n; i++) {
        const Document *d = &s->documents.v[i];
        in[i].doc_num = kb_id_num(d->id, 'D');
        in[i].chunk_base = d->chunk_base;
        in[i].chunk_count = d->chunk_count;
        in[i].lang = doc_lang(d->mime, d->path);
        /* A missing blob is a damaged store, not a reason to refuse to
         * index the rest of it: the document keeps its place in the log
         * order the digest is taken over, and contributes no chunks. */
        char *text;
        size_t len;
        if (store_get_blob(s, d->content_hash, &text, &len)) {
            in[i].text = text;
            in[i].len = len;
        } else {
            in[i].text = "";
            in[i].len = 0;
            in[i].chunk_count = 0;
            (*missing_blobs)++;
        }
    }
    char digest[65];
    fts_store_digest(s, cp, digest);
    size_t image_len = 0;
    char *image = fts_build(a, in, n,
                            (size_t)cp.chunk_tokens * KB_BYTES_PER_TOKEN,
                            (size_t)cp.chunk_overlap * KB_BYTES_PER_TOKEN,
                            digest, &image_len, stats, err, errsz);
    if (!image)
        return false;
    char path[KB_PATH_MAX];
    fts_path(s, path, sizeof path);
    if (!plat_mkdirs(s->index_dir)) {
        snprintf(err, errsz, "cannot create %s", s->index_dir);
        return false;
    }
    /* Written through a temp file and renamed: a crash leaves either the
     * old index or the new one, never half of one. A half-written index
     * would still be detected — the header states the file's own size — but
     * detected corruption is worse service than none. */
    if (!plat_write_file_atomic(path, image, image_len)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    *docs = (uint32_t)n;
    return true;
}

/* ---- derived source facts --------------------------------------------- */

SourceFacts source_facts(Arena *a, const Store *s, const char *source_id) {
    SourceFacts f;
    memset(&f, 0, sizeof f);
    const char *single_hash = NULL;
    Sha256 c;
    sha256_init(&c);
    for (size_t i = 0; i < s->documents.n; i++) {
        const Document *d = &s->documents.v[i];
        if (strcmp(d->source, source_id) != 0)
            continue;
        f.doc_count++;
        f.bytes += d->bytes;
        single_hash = d->content_hash;
        sha256_update(&c, d->content_hash, strlen(d->content_hash));
        if (!f.fetched_at ||
            (d->fetched_at && strcmp(d->fetched_at, f.fetched_at) > 0))
            f.fetched_at = d->fetched_at;
    }
    if (f.doc_count == 1) {
        f.content_hash = single_hash;
    } else if (f.doc_count > 1) {
        /* A manifest hash over the documents' hashes. Documents are folded
         * in log order, which is stable, so the same corpus always produces
         * the same digest. */
        uint8_t digest[32];
        sha256_final(&c, digest);
        char *hex = (char *)arena_alloc(a, 65);
        static const char hexd[] = "0123456789abcdef";
        for (int32_t i = 0; i < 32; i++) {
            hex[i * 2] = hexd[digest[i] >> 4];
            hex[i * 2 + 1] = hexd[digest[i] & 0xf];
        }
        hex[64] = '\0';
        f.content_hash = hex;
    }
    return f;
}

/* ---- shared JSON shapes ----------------------------------------------- */

void json_document(StrBuf *sb, const Document *d, const Source *src,
                   const Staleness *st) {
    sb_printf(sb, "\"id\":\"%s\",\"source\":\"%s\"", d->id, d->source);
    sb_puts(sb, ",\"collection\":");
    /* A document's collection is its source's: §1.3 gives a document exactly
     * one, and §1.2 puts it on the Source. Storing it twice would let the
     * two disagree. */
    json_escape_c(sb, src ? src->collection : "");
    sb_puts(sb, ",\"path\":");
    json_escape_c(sb, d->path);
    sb_puts(sb, ",\"title\":");
    json_escape_c(sb, d->title ? d->title : "");
    sb_puts(sb, ",\"mime\":");
    json_escape_c(sb, d->mime ? d->mime : "");
    sb_puts(sb, ",\"locator\":");
    json_escape_c(sb, src ? src->locator : "");
    sb_printf(sb, ",\"contentHash\":\"%s\",\"bytes\":%llu", d->content_hash,
              (unsigned long long)d->bytes);
    sb_printf(sb, ",\"fetchedAt\":\"%s\",\"indexedAt\":\"%s\"",
              d->fetched_at ? d->fetched_at : "",
              d->indexed_at ? d->indexed_at : "");
    /* §5 puts `stale` beside `fetchedAt` on a hit; a row that carries the
     * date must carry the verdict too, or `ls` and `search` would answer the
     * same question differently. */
    sb_printf(sb, ",\"stale\":%s", doc_stale(st, d) ? "true" : "false");
    sb_printf(sb, ",\"chunkCount\":%lu,\"chunkBase\":%lld",
              (unsigned long)d->chunk_count, (long long)d->chunk_base);
    sb_puts(sb, ",\"meta\":");
    sb_puts(sb, d->meta ? d->meta : "{}");
}

/* ---- links (§6) -------------------------------------------------------- */

void json_links(StrBuf *sb, const Store *s, const char *id, bool outgoing,
                const Staleness *st) {
    sb_putc(sb, '[');
    bool first = true;
    for (size_t i = 0; i < s->documents.nlinks; i++) {
        const Link *l = &s->documents.links[i];
        const char *this_end = outgoing ? l->from : l->to;
        const char *far = outgoing ? l->to : l->from;
        if (strcmp(this_end, id) != 0)
            continue;
        if (!first)
            sb_putc(sb, ',');
        first = false;
        /* BOTH ends on every row, not just the far one. A caller holding a
         * row would otherwise have to remember which list it came out of to
         * know which way the edge points, and a row that cannot be read on
         * its own is a row that gets read wrong. */
        sb_printf(sb, "{\"type\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"", l->rel,
                  l->from, l->to);
        const Document *d = doc_by_id(&s->documents, far);
        /* §6 says "resolved to rows". A row whose far end is gone is still a
         * row: the edge exists in the log and hiding it would turn a
         * dangling link into an invisible one. */
        sb_printf(sb, ",\"resolved\":%s", d ? "true" : "false");
        if (d) {
            sb_puts(sb, ",\"document\":{");
            json_document(sb, d, src_by_id(&s->sources, d->source), st);
            sb_putc(sb, '}');
        } else {
            sb_puts(sb, ",\"document\":null");
        }
        sb_puts(sb, ",\"createdAt\":");
        if (l->created_at)
            json_escape_c(sb, l->created_at);
        else
            sb_puts(sb, "null");
        sb_putc(sb, '}');
    }
    sb_putc(sb, ']');
}

void json_source(StrBuf *sb, const Store *s, const Source *src) {
    SourceFacts f = source_facts(s->a, s, src->id);
    sb_printf(sb, "\"id\":\"%s\",\"kind\":\"%s\"", src->id, src->kind);
    sb_puts(sb, ",\"locator\":");
    json_escape_c(sb, src->locator);
    sb_puts(sb, ",\"title\":");
    json_escape_c(sb, src->title ? src->title : "");
    sb_puts(sb, ",\"collection\":");
    json_escape_c(sb, src->collection);
    sb_printf(sb, ",\"fetchedAt\":\"%s\"", f.fetched_at ? f.fetched_at : "");
    sb_printf(sb, ",\"contentHash\":\"%s\"",
              f.content_hash ? f.content_hash : "");
    sb_printf(sb, ",\"docCount\":%lu,\"bytes\":%llu,\"status\":\"ok\"",
              (unsigned long)f.doc_count, (unsigned long long)f.bytes);
}
