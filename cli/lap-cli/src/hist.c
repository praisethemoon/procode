#include "hist.h"

#include "sha256.h"

static bool is_hex_lower(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool hist_parse_name(const char *name, char lineage[HIST_LINEAGE_MAX],
                     int32_t *n) {
    const char *dot = strchr(name, '.');
    if (!dot)
        return false;
    size_t llen = (size_t)(dot - name);
    bool main_lineage = llen == strlen(LAP_MAIN_LINEAGE) &&
                        memcmp(name, LAP_MAIN_LINEAGE, llen) == 0;
    if (!main_lineage) {
        if (llen != 12)
            return false;
        for (size_t i = 0; i < llen; i++) {
            if (!is_hex_lower(name[i]))
                return false;
        }
    }
    const char *num = dot + 1;
    for (int32_t i = 0; i < 6; i++) {
        if (num[i] < '0' || num[i] > '9')
            return false;
    }
    if (strcmp(num + 6, ".jsonl") != 0)
        return false;
    int32_t v = (int32_t)strtol(num, NULL, 10);
    if (v < 1)
        return false;
    memcpy(lineage, name, llen);
    lineage[llen] = '\0';
    *n = v;
    return true;
}

void hist_chunk_name(const char *lineage, int32_t n, char out[64]) {
    snprintf(out, 64, "%s.%06d.jsonl", lineage, n);
}

uint64_t hist_chunk_limit(void) {
    const char *env = getenv("LAP_TEST_CHUNK_BYTES");
    if (env && env[0]) {
        char *end;
        unsigned long long v = strtoull(env, &end, 10);
        if (*end == '\0' && v > 0)
            return (uint64_t)v;
    }
    return LAP_CHUNK_BYTES;
}

typedef struct {
    Arena *a;
    Hist *h;
    const char *lineage;
} ListCtx;

static WalkAction on_log_entry(const char *rel, bool is_dir,
                               const PlatStat *st, void *ud) {
    ListCtx *c = (ListCtx *)ud;
    if (is_dir)
        return WALK_SKIP_DIR;
    char lineage[HIST_LINEAGE_MAX];
    int32_t n;
    if (!hist_parse_name(rel, lineage, &n) ||
        strcmp(lineage, c->lineage) != 0)
        return WALK_CONT;
    Hist *h = c->h;
    if (h->n == h->cap) {
        int32_t ncap = h->cap ? h->cap * 2 : 8;
        h->v = (HistChunk *)arena_realloc(c->a, h->v,
                                          (size_t)h->cap * sizeof(HistChunk),
                                          (size_t)ncap * sizeof(HistChunk));
        h->cap = ncap;
    }
    HistChunk *k = &h->v[h->n++];
    memset(k, 0, sizeof *k);
    snprintf(k->lineage, sizeof k->lineage, "%s", lineage);
    k->n = n;
    snprintf(k->name, sizeof k->name, "%s", rel);
    k->size = st ? st->size : 0;
    return WALK_CONT;
}

/* hist_open without the old single-file log: the lineage's chunks only. */
static bool list_chunks(Arena *a, const char *lapdir, const char *lineage,
                        Hist *h, char *err, size_t errsz) {
    memset(h, 0, sizeof *h);
    snprintf(h->dir, sizeof h->dir, "%s/%s", lapdir, LAP_LOG_DIR);
    snprintf(h->lineage, sizeof h->lineage, "%s", lineage);
    h->limit = hist_chunk_limit();
    ListCtx c = {a, h, lineage};
    if (plat_is_dir(h->dir) && !plat_walk(a, h->dir, on_log_entry, &c)) {
        snprintf(err, errsz, "cannot list %s", h->dir);
        return false;
    }
    /* the walk sorts by name, and the zero-padded numbers sort as numbers */
    uint64_t start = 0;
    for (int32_t i = 0; i < h->n; i++) {
        if (h->v[i].n != i + 1) {
            snprintf(err, errsz,
                     "history chunk %s.%06d.jsonl is missing from %s",
                     lineage, i + 1, h->dir);
            return false;
        }
        h->v[i].start = start;
        start += h->v[i].size;
    }
    h->size = start;
    return true;
}

bool hist_open(Arena *a, const char *lapdir, const char *lineage, Hist *h,
               char *err, size_t errsz) {
    if (!list_chunks(a, lapdir, lineage, h, err, errsz))
        return false;
    if (strcmp(lineage, LAP_MAIN_LINEAGE) != 0)
        return true;
    char legacy[LAP_PATH_MAX];
    snprintf(legacy, sizeof legacy, "%s/%s", lapdir, LAP_LOG_NAME);
    uint64_t size;
    if (!plat_file_size(legacy, &size))
        return true;
    /* Both shapes: a conversion between publishing its chunks and removing
     * the old file (they hold all of it), or an older lap's interrupted
     * one (they do not). The old file is read unless the chunks hold all
     * of it — never a history cut short. */
    if (h->n > 0) {
        char *old, *have;
        size_t old_len;
        if (!plat_read_file_max(a, legacy, &old, &old_len, (size_t)-1)) {
            snprintf(err, errsz, "cannot read %s", legacy);
            return false;
        }
        while (old_len > 0 && old[old_len - 1] != '\n')
            old_len--;
        if (h->size >= old_len &&
            hist_read(a, h, 0, old_len, &have) &&
            memcmp(have, old, old_len) == 0)
            return true;
        memset(h, 0, sizeof *h);
        snprintf(h->dir, sizeof h->dir, "%s/%s", lapdir, LAP_LOG_DIR);
        snprintf(h->lineage, sizeof h->lineage, "%s", lineage);
        h->limit = hist_chunk_limit();
    }
    char *mem;
    size_t mlen;
    if (!plat_read_file_max(a, legacy, &mem, &mlen, (size_t)-1)) {
        /* gone since: a conversion finished in between; its chunks hold
         * all of it */
        return list_chunks(a, lapdir, lineage, h, err, errsz);
    }
    size = mlen;
    h->mem = mem;
    h->legacy = true;
    h->v = (HistChunk *)arena_alloc0(a, sizeof(HistChunk));
    h->cap = 1;
    h->n = 1;
    snprintf(h->v[0].lineage, sizeof h->v[0].lineage, "%s", lineage);
    h->v[0].n = 1;
    snprintf(h->v[0].name, sizeof h->v[0].name, "%s", LAP_LOG_NAME);
    h->v[0].size = size;
    h->size = size;
    return true;
}

void hist_chunk_path(const Hist *h, int32_t i, char *out, size_t outsz) {
    if (h->legacy) { /* <lapdir>/log.jsonl, beside the log directory */
        size_t lapdir_len = strlen(h->dir) - strlen("/" LAP_LOG_DIR);
        snprintf(out, outsz, "%.*s/%s", (int)lapdir_len, h->dir,
                 h->v[i].name);
        return;
    }
    snprintf(out, outsz, "%s/%s", h->dir, h->v[i].name);
}

int32_t hist_locate(const Hist *h, uint64_t off) {
    if (h->n == 0 || off > h->size)
        return -1;
    int32_t lo = 0, hi = h->n - 1;
    while (lo < hi) { /* last chunk whose start <= off */
        int32_t mid = lo + (hi - lo + 1) / 2;
        if (h->v[mid].start <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

const char *hist_label(const Hist *h, int32_t i) {
    const char *lineage = h->v[i].lineage;
    if (h->v[i].label)
        return h->v[i].label;
    if (h->parent[0] && strcmp(lineage, h->lineage) == 0)
        return h->name;
    return lineage;
}

bool hist_is_sealed(const Hist *h, int32_t i) {
    return i >= 0 && i < h->n - 1;
}

void hist_where(const Hist *h, const char *data, uint64_t off, char *out,
                size_t outsz) {
    int32_t i = -1; /* the chunk holding the byte, else the last with any */
    for (int32_t k = 0; k < h->n; k++) {
        if (h->v[k].size == 0)
            continue;
        i = k;
        if (off < h->v[k].start + h->v[k].size)
            break;
    }
    if (i < 0) {
        snprintf(out, outsz, "history offset %llu", (unsigned long long)off);
        return;
    }
    int32_t line = 1;
    for (uint64_t p = h->v[i].start; p < off; p++) {
        if (data[p] == '\n')
            line++;
    }
    snprintf(out, outsz, "%s line %d", h->v[i].name, line);
}

bool hist_read(Arena *a, const Hist *h, uint64_t off, size_t len, char **out) {
    if (off + len > h->size)
        return false;
    char *buf = (char *)arena_alloc(a, len + 1);
    if (h->mem) { /* the old single file, read when opened */
        memcpy(buf, h->mem + off, len);
        buf[len] = '\0';
        *out = buf;
        return true;
    }
    size_t got = 0;
    int32_t i = hist_locate(h, off);
    while (got < len && i >= 0 && i < h->n) {
        const HistChunk *k = &h->v[i];
        uint64_t at = off + got;
        if (at >= k->start + k->size) {
            i++;
            continue;
        }
        uint64_t in = at - k->start;
        size_t take = (size_t)(k->size - in);
        if (take > len - got)
            take = len - got;
        char path[LAP_PATH_MAX];
        hist_chunk_path(h, i, path, sizeof path);
        if (!plat_read_range_into(path, in, take, buf + got))
            return false;
        got += take;
        i++;
    }
    if (got != len)
        return false;
    buf[len] = '\0';
    *out = buf;
    return true;
}

bool hist_read_all(Arena *a, const Hist *h, char **data, size_t *len) {
    *len = (size_t)h->size;
    return hist_read(a, h, 0, (size_t)h->size, data);
}

/* The hash of one file's last complete line, read through a tail window
 * that doubles until the line fits — never the whole file. *found is false
 * when the file holds no complete line. */
static bool file_tail_hash(Arena *a, const char *path, char out[65],
                           bool *found) {
    *found = false;
    size_t window = 64 * 1024;
    for (;;) {
        char *data;
        size_t len;
        uint64_t fsize;
        if (!plat_read_tail(a, path, window, &data, &len, &fsize))
            return false;
        if (len == 0)
            return true;
        bool whole_file = (uint64_t)len == fsize;
        size_t end = len; /* skip a torn (unterminated) tail */
        while (end > 0 && data[end - 1] != '\n')
            end--;
        if (end == 0) {
            if (whole_file)
                return true;
            window *= 2; /* the torn line alone exceeds the window */
            continue;
        }
        /* end is just past the last complete line's '\n'; skip blank
         * lines above it */
        size_t line_end = end - 1;
        while (line_end > 0 && data[line_end - 1] == '\n')
            line_end--;
        if (line_end == 0) {
            if (whole_file)
                return true;
            window *= 2;
            continue;
        }
        size_t start = line_end;
        while (start > 0 && data[start - 1] != '\n')
            start--;
        if (start == 0 && !whole_file) {
            window *= 2; /* the line may begin before the window */
            continue;
        }
        sha256_hex(data + start, line_end - start, out);
        *found = true;
        return true;
    }
}

bool hist_tail_hash(Arena *a, const Hist *h, char out[65]) {
    if (h->mem) { /* the old single file, read when opened */
        size_t end = (size_t)h->size;
        while (end > 0 && h->mem[end - 1] != '\n')
            end--; /* a torn tail */
        size_t line_end = end > 0 ? end - 1 : 0;
        while (line_end > 0 && h->mem[line_end - 1] == '\n')
            line_end--;
        size_t start = line_end;
        while (start > 0 && h->mem[start - 1] != '\n')
            start--;
        if (line_end > start)
            sha256_hex(h->mem + start, line_end - start, out);
        else
            snprintf(out, 65, "%s", LAP_HASH_ZERO);
        return true;
    }
    for (int32_t i = h->n - 1; i >= 0; i--) {
        if (h->v[i].size == 0)
            continue;
        char path[LAP_PATH_MAX];
        hist_chunk_path(h, i, path, sizeof path);
        bool found;
        if (!file_tail_hash(a, path, out, &found))
            return false;
        if (found)
            return true;
    }
    snprintf(out, 65, "%s", LAP_HASH_ZERO);
    return true;
}

/* The index of this folder's open chunk: the last chunk, when it belongs to
 * the lineage this folder writes; -1 before that lineage has a chunk. */
static int32_t open_chunk(const Hist *h) {
    if (h->n > 0 && strcmp(h->v[h->n - 1].lineage, h->lineage) == 0)
        return h->n - 1;
    return -1;
}

bool hist_repair_torn_tail(Arena *a, Hist *h) {
    int32_t i = open_chunk(h);
    if (i < 0 || h->v[i].size == 0)
        return true;
    char path[LAP_PATH_MAX];
    hist_chunk_path(h, i, path, sizeof path);
    size_t window = 64 * 1024;
    for (;;) {
        char *data;
        size_t len;
        uint64_t fsize;
        if (!plat_read_tail(a, path, window, &data, &len, &fsize))
            return false;
        if (len == 0 || data[len - 1] == '\n')
            return true;
        size_t end = len;
        while (end > 0 && data[end - 1] != '\n')
            end--;
        if (end == 0 && (uint64_t)len != fsize) {
            window *= 2;
            continue;
        }
        uint64_t keep = fsize - (uint64_t)(len - end);
        uint64_t dropped = fsize - keep;
        if (!plat_truncate(path, keep))
            return false;
        fprintf(stderr,
                "lap: repaired torn log tail (%llu bytes from an "
                "interrupted append dropped)\n",
                (unsigned long long)dropped);
        h->v[i].size = keep;
        h->size = h->v[i].start + keep;
        return true;
    }
}

/* Creates the lineage's chunk n, empty, and lists it. */
static bool new_chunk(Arena *a, Hist *h, int32_t n, char *err, size_t errsz) {
    /* six digits name a chunk: past the last, a chunk would be one no
     * reader lists (about 4 TB of history at the chunk limit) */
    if (n > HIST_MAX_CHUNK) {
        snprintf(err, errsz,
                 "the history in %s has reached its last chunk number (%d)",
                 h->dir, HIST_MAX_CHUNK);
        return false;
    }
    if (!plat_mkdirs(h->dir)) {
        snprintf(err, errsz, "cannot create %s", h->dir);
        return false;
    }
    if (h->n == h->cap) {
        int32_t ncap = h->cap ? h->cap * 2 : 8;
        h->v = (HistChunk *)arena_realloc(a, h->v,
                                          (size_t)h->cap * sizeof(HistChunk),
                                          (size_t)ncap * sizeof(HistChunk));
        h->cap = ncap;
    }
    HistChunk *k = &h->v[h->n];
    memset(k, 0, sizeof *k);
    snprintf(k->lineage, sizeof k->lineage, "%s", h->lineage);
    k->n = n;
    hist_chunk_name(h->lineage, n, k->name);
    k->start = h->size;
    char path[LAP_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", h->dir, k->name);
    /* the new name made durable too: records appended to a chunk whose
     * entry a power loss dropped would be lost with it */
    if (!plat_append_file_sync(path, "", 0) || !plat_fsync_dir(h->dir)) {
        snprintf(err, errsz, "cannot create %s", path);
        return false;
    }
    h->n++;
    return true;
}

bool hist_seal(Arena *a, Hist *h, char *err, size_t errsz) {
    int32_t i = open_chunk(h);
    if (i < 0 || h->v[i].size == 0)
        return true;
    return new_chunk(a, h, h->v[i].n + 1, err, errsz);
}

bool hist_append(Arena *a, Hist *h, const char *line, size_t len, char *err,
                 size_t errsz) {
    int32_t i = open_chunk(h);
    if (i < 0) {
        if (!new_chunk(a, h, 1, err, errsz))
            return false;
        i = h->n - 1;
    } else if (h->v[i].size > 0 &&
               h->v[i].size + (uint64_t)len > h->limit) {
        if (!new_chunk(a, h, h->v[i].n + 1, err, errsz))
            return false;
        i = h->n - 1;
    }
    char path[LAP_PATH_MAX];
    hist_chunk_path(h, i, path, sizeof path);
    if (!plat_append_file_sync(path, line, len)) {
        snprintf(err, errsz, "cannot append to %s", path);
        return false;
    }
    h->v[i].size += (uint64_t)len;
    h->size += (uint64_t)len;
    return true;
}

/* The names of the files directly in a folder. */
typedef struct {
    Arena *a;
    const char **v;
    int32_t n, cap;
} Names;

static WalkAction on_name(const char *rel, bool is_dir, const PlatStat *st,
                          void *ud) {
    (void)st;
    Names *ns = (Names *)ud;
    if (is_dir)
        return WALK_SKIP_DIR;
    ARENA_GROW(ns->a, ns->v, ns->n, ns->cap, const char *);
    ns->v[ns->n++] = arena_strdup(ns->a, rel);
    return WALK_CONT;
}

/* Removes a folder of files and the folder, if it is there. */
static void clear_dir(Arena *a, const char *dir) {
    if (!plat_is_dir(dir))
        return;
    Names ns = {a, NULL, 0, 0};
    plat_walk(a, dir, on_name, &ns);
    for (int32_t i = 0; i < ns.n; i++) {
        char path[LAP_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, ns.v[i]);
        plat_remove_file(path);
    }
    plat_rmdir(dir);
}

/* Moves the chunks of lineages other than main from one folder to
 * another. */
static void move_other_lineages(Arena *a, const char *from, const char *to) {
    Names ns = {a, NULL, 0, 0};
    plat_walk(a, from, on_name, &ns);
    for (int32_t i = 0; i < ns.n; i++) {
        char lineage[HIST_LINEAGE_MAX];
        int32_t k;
        if (!hist_parse_name(ns.v[i], lineage, &k) ||
            strcmp(lineage, LAP_MAIN_LINEAGE) == 0)
            continue;
        char src[LAP_PATH_MAX], dst[LAP_PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", from, ns.v[i]);
        snprintf(dst, sizeof dst, "%s/%s", to, ns.v[i]);
        plat_rename(src, dst);
    }
}

/* Whether the chunks' bytes past the old file (have, from old_len on) are
 * leftovers of a conversion run under another chunk limit rather than
 * records appended since: they break the chain there, and every one of
 * them is a record the old file already holds. Bytes that do not parse
 * are not judged here. */
static bool stale_extension(Arena *a, const char *have, size_t have_len,
                            size_t old_len) {
    RecLog all;
    char err[128];
    if (have_len == old_len ||
        !rec_log_parse(a, have, have_len, NULL, NULL, &all, err, sizeof err) ||
        all.chain_ok)
        return false;
    StrSet held;
    strset_init(&held, a);
    int32_t i = 0;
    for (; i < all.count && (size_t)(all.v[i].raw - have) < old_len; i++)
        strset_add(&held, all.v[i].hash);
    for (; i < all.count; i++) {
        if (!strset_has(&held, all.v[i].hash))
            return false;
    }
    return true;
}

bool hist_convert_legacy(Arena *a, const char *lapdir, uint64_t limit,
                         bool *converted, char *err, size_t errsz) {
    *converted = false;
    char legacy[LAP_PATH_MAX];
    snprintf(legacy, sizeof legacy, "%s/%s", lapdir, LAP_LOG_NAME);
    if (!plat_is_file(legacy))
        return true;
    char *old;
    size_t old_len;
    if (!plat_read_file_max(a, legacy, &old, &old_len, (size_t)-1)) {
        snprintf(err, errsz, "cannot read %s", legacy);
        return false;
    }
    size_t whole = old_len; /* a torn final line was never acknowledged */
    while (old_len > 0 && old[old_len - 1] != '\n')
        old_len--;

    Hist h; /* the chunks as they are, not hist_open's choice between shapes */
    if (!list_chunks(a, lapdir, LAP_MAIN_LINEAGE, &h, err, errsz))
        return false;
    if (h.n > 0) {
        char *have;
        size_t have_len;
        if (!hist_read_all(a, &h, &have, &have_len)) {
            snprintf(err, errsz, "cannot read the history in %s", h.dir);
            return false;
        }
        bool starts_with_old =
            have_len >= old_len && memcmp(have, old, old_len) == 0;
        if (starts_with_old && !stale_extension(a, have, have_len, old_len)) {
            /* the conversion finished; only the removal did not */
            if (!plat_remove_file(legacy)) {
                snprintf(err, errsz, "cannot remove %s", legacy);
                return false;
            }
            return true;
        }
        if (!starts_with_old &&
            (have_len > old_len || memcmp(have, old, have_len) != 0)) {
            snprintf(err, errsz,
                     "both %s and %s hold history, and they differ: keep "
                     "the one that is right and move the other away",
                     legacy, h.dir);
            return false;
        }
        /* chunks that are a prefix of the old file, or the whole of it
         * with stale leftovers after: an interrupted conversion, written
         * again below */
    }

    /* The chunks are built in a folder of their own and published as log/
     * with one rename, so a reader finds the old file or every chunk,
     * never some. Fixed names: conversion runs under the lock, and
     * leftovers of an interrupted run are cleared first. */
    char dir[LAP_PATH_MAX], tmp[LAP_PATH_MAX], aside[LAP_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", lapdir, LAP_LOG_DIR);
    snprintf(tmp, sizeof tmp, "%s/log.converting", lapdir);
    snprintf(aside, sizeof aside, "%s/log.replaced", lapdir);
    clear_dir(a, tmp);
    clear_dir(a, aside);
    if (!plat_mkdirs(tmp)) {
        snprintf(err, errsz, "cannot create %s", tmp);
        return false;
    }
    int32_t n = 0;
    size_t start = 0, pos = 0;
    while (pos < old_len || start < pos) {
        size_t end = pos;
        while (end < old_len && old[end] != '\n')
            end++;
        size_t next = end < old_len ? end + 1 : end;
        bool full = pos > start && (uint64_t)(next - start) > limit;
        if (full || pos >= old_len) {
            char name[64], path[LAP_PATH_MAX];
            hist_chunk_name(LAP_MAIN_LINEAGE, ++n, name);
            snprintf(path, sizeof path, "%s/%s", tmp, name);
            if (!plat_write_file_atomic(path, old + start, pos - start)) {
                snprintf(err, errsz, "cannot write %s", path);
                return false;
            }
            start = pos;
            if (pos >= old_len)
                break;
        }
        pos = next;
    }
    if (n == 0) {
        /* an old file with no complete record is a history with nothing in
         * it yet, not none: it becomes one empty chunk, which the first
         * write fills as it would have filled the old file */
        char name[64], path[LAP_PATH_MAX];
        hist_chunk_name(LAP_MAIN_LINEAGE, ++n, name);
        snprintf(path, sizeof path, "%s/%s", tmp, name);
        if (!plat_write_file_atomic(path, "", 0)) {
            snprintf(err, errsz, "cannot write %s", path);
            return false;
        }
    }
    /* an existing log/ (an older lap's partial conversion) goes aside
     * whole, its other lineages' chunks carried over; meanwhile readers
     * see only the old file, which is complete */
    if (plat_is_dir(dir)) {
        move_other_lineages(a, dir, tmp);
        if (!plat_rename(dir, aside)) {
            snprintf(err, errsz, "cannot move %s aside", dir);
            return false;
        }
    }
    /* the chunks' names durable in the working folder, then log/'s in
     * .lap/, before the old file goes: a power loss never leaves neither */
    if (!plat_fsync_dir(tmp)) {
        snprintf(err, errsz, "cannot make %s durable", tmp);
        return false;
    }
    if (!plat_rename(tmp, dir) || !plat_fsync_dir(lapdir)) {
        snprintf(err, errsz, "cannot publish %s as %s", tmp, dir);
        return false;
    }
    clear_dir(a, aside);
    if (!plat_remove_file(legacy)) {
        snprintf(err, errsz, "cannot remove %s", legacy);
        return false;
    }
    if (whole != old_len)
        fprintf(stderr,
                "lap: repaired torn log tail (%llu bytes from an "
                "interrupted append dropped)\n",
                (unsigned long long)(whole - old_len));
    fprintf(stderr, "lap: moved .lap/%s into %d chunk%s in .lap/%s/\n",
            LAP_LOG_NAME, n, n == 1 ? "" : "s", LAP_LOG_DIR);
    *converted = true;
    return true;
}

bool hist_folder_lineage(Arena *a, const char *lapdir,
                         char out[HIST_LINEAGE_MAX], char *err, size_t errsz) {
    char path[LAP_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", lapdir, LAP_LINEAGE_NAME);
    char *data;
    size_t len;
    if (!plat_is_file(path)) {
        snprintf(out, HIST_LINEAGE_MAX, "%s", LAP_MAIN_LINEAGE);
        return true;
    }
    if (!plat_read_file(a, path, &data, &len)) {
        snprintf(err, errsz, "cannot read %s", path);
        return false;
    }
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r' ||
                       data[len - 1] == ' '))
        len--;
    char name[64];
    int32_t n;
    snprintf(name, sizeof name, "%.*s.000001.jsonl", (int)len, data);
    if (len != 12 || !hist_parse_name(name, out, &n)) {
        snprintf(err, errsz, "%s does not hold a branch id", path);
        return false;
    }
    /* A lineage whose recorded parent is this folder itself came here
     * through git from its branch (with .lap/parent): this folder is not
     * that branch, whatever the file says. */
    char ppath[LAP_PATH_MAX], root[LAP_PATH_MAX];
    snprintf(ppath, sizeof ppath, "%s/%s", lapdir, LAP_PARENT_NAME);
    snprintf(root, sizeof root, "%s", lapdir);
    size_t rl = strlen(root), dl = strlen("/" LAP_DIR);
    if (rl > dl && strcmp(root + rl - dl, "/" LAP_DIR) == 0)
        root[rl - dl] = '\0';
    char *pdata;
    size_t plen;
    if (plat_is_file(ppath) && plat_read_file(a, ppath, &pdata, &plen)) {
        while (plen > 0 && (pdata[plen - 1] == '\n' || pdata[plen - 1] == '\r'))
            plen--;
        pdata[plen] = '\0';
        if (plen > 0 && plat_same_file(pdata, root)) {
            static bool told; /* once a command */
            if (!told)
                fprintf(stderr,
                        "note: ignoring %s: it names branch %s, whose parent "
                        "is this folder itself (it came through git?); this "
                        "folder is main. Remove .lap/lineage and .lap/parent "
                        "here.\n",
                        path, out);
            told = true;
            snprintf(out, HIST_LINEAGE_MAX, "%s", LAP_MAIN_LINEAGE);
        }
    }
    return true;
}

bool hist_write_chunk(const char *logdir, const char *name, const void *data,
                      size_t len) {
    /* its temp file goes in .lap/, next to log/: git and readers never
     * see one in log/ */
    char path[LAP_PATH_MAX], lap[LAP_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", logdir, name);
    snprintf(lap, sizeof lap, "%s", logdir);
    char *slash = strrchr(lap, '/');
    if (slash)
        *slash = '\0';
    return plat_write_file_atomic_in(path, slash ? lap : NULL, data, len) &&
           plat_fsync_dir(logdir);
}

void hist_clear_tmp(Arena *a, const char *lapdir) {
    char dir[LAP_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", lapdir, LAP_LOG_DIR);
    if (!plat_is_dir(dir))
        return;
    Names ns = {a, NULL, 0, 0};
    plat_walk(a, dir, on_name, &ns);
    for (int32_t i = 0; i < ns.n; i++) {
        if (!plat_is_tmp_name(ns.v[i]))
            continue;
        char path[LAP_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, ns.v[i]);
        plat_remove_file(path);
    }
}

bool hist_write_lineage(const char *lapdir, const char *lineage) {
    char path[LAP_PATH_MAX], text[HIST_LINEAGE_MAX + 1];
    snprintf(path, sizeof path, "%s/%s", lapdir, LAP_LINEAGE_NAME);
    snprintf(text, sizeof text, "%s\n", lineage);
    return plat_write_file_atomic(path, text, strlen(text)) &&
           plat_fsync_dir(lapdir);
}

/* The first record of the chunk at path (named name, for messages), read
 * through a head window that doubles until its line fits. */
static bool chunk_first_record(Arena *a, const char *path, const char *name,
                               uint64_t size, Rec *out, char *err,
                               size_t errsz) {
    size_t want = size < 65536 ? (size_t)size : 65536;
    for (;;) {
        char *data;
        if (!plat_read_range(a, path, 0, want, &data)) {
            snprintf(err, errsz, "cannot read %s", path);
            return false;
        }
        char *nl = memchr(data, '\n', want);
        if (nl) {
            char derr[200];
            if (!rec_decode(a, data, (size_t)(nl - data), out, derr,
                            sizeof derr)) {
                snprintf(err, errsz, "%s line 1: %s", name, derr);
                return false;
            }
            return true;
        }
        if ((uint64_t)want >= size) {
            snprintf(err, errsz, "%s holds no complete record", name);
            return false;
        }
        want = (uint64_t)want * 2 < size ? want * 2 : (size_t)size;
    }
}

bool hist_first_record(Arena *a, const char *lapdir, const char *lineage,
                       Rec *out, char *err, size_t errsz) {
    char name[64], path[LAP_PATH_MAX];
    hist_chunk_name(lineage, 1, name);
    snprintf(path, sizeof path, "%s/%s/%s", lapdir, LAP_LOG_DIR, name);
    uint64_t size;
    if (!plat_file_size(path, &size) || size == 0) {
        snprintf(err, errsz, "the history of %s starts nowhere: %s is "
                             "missing or empty",
                 lineage, path);
        return false;
    }
    return chunk_first_record(a, path, name, size, out, err, errsz);
}

bool hist_check(Arena *a, const Hist *h, bool chain, char *err,
                size_t errsz) {
    int32_t prev = -1; /* the last chunk before i that holds records */
    for (int32_t i = 0; i < h->n; i++) {
        const HistChunk *k = &h->v[i];
        if (k->size == 0)
            continue;
        char path[LAP_PATH_MAX];
        hist_chunk_path(h, i, path, sizeof path);
        if (i < h->n - 1) { /* sealed: whole lines only */
            char *last;
            if (!plat_read_range(a, path, k->size - 1, 1, &last)) {
                snprintf(err, errsz, "cannot read %s", path);
                return false;
            }
            if (last[0] != '\n') {
                snprintf(err, errsz,
                         "sealed chunk %s ends in a torn line: it was cut or "
                         "copied short (lap never writes a sealed chunk)",
                         k->name);
                return false;
            }
        }
        if (chain && prev >= 0) {
            char ppath[LAP_PATH_MAX], tail[65];
            bool found;
            Rec first;
            char ferr[256];
            hist_chunk_path(h, prev, ppath, sizeof ppath);
            if (!file_tail_hash(a, ppath, tail, &found) ||
                !chunk_first_record(a, path, k->name, k->size, &first, ferr,
                                    sizeof ferr) ||
                !found || strcmp(first.prev, tail) != 0) {
                snprintf(err, errsz, "%s does not continue %s: the hash "
                                     "chain is broken there",
                         k->name, h->v[prev].name);
                return false;
            }
        }
        prev = i;
    }
    return true;
}

bool hist_open_folder(Arena *a, const char *lapdir, Hist *h, char *err,
                      size_t errsz) {
    char lineage[HIST_LINEAGE_MAX];
    if (!hist_folder_lineage(a, lapdir, lineage, err, errsz))
        return false;
    if (strcmp(lineage, LAP_MAIN_LINEAGE) == 0)
        return hist_open(a, lapdir, lineage, h, err, errsz);
    return hist_open_lineage(a, lapdir, lineage, h, err, errsz);
}

/* Branches nest at most this deep: a longer chain of branch records is a
 * loop, not a history. */
#define HIST_MAX_DEPTH 32

/* hist_open_view, depth branch records into the chain. */
static bool open_view(Arena *a, const char *lapdir, const char *lineage,
                      int32_t upto, int32_t depth, Hist *h, char *err,
                      size_t errsz) {
    if (strcmp(lineage, LAP_MAIN_LINEAGE) == 0) {
        if (!hist_open(a, lapdir, lineage, h, err, errsz))
            return false;
        if (upto > 0 && (h->legacy || h->n < upto)) {
            char name[64];
            hist_chunk_name(lineage, upto, name);
            snprintf(err, errsz, "%s is missing from %s", name, h->dir);
            return false;
        }
        if (upto > 0) {
            h->n = upto;
            h->size = h->v[upto - 1].start + h->v[upto - 1].size;
        }
        return true;
    }
    if (depth >= HIST_MAX_DEPTH) {
        snprintf(err, errsz, "branch records in %s form a loop", lapdir);
        return false;
    }
    Rec br;
    if (!hist_first_record(a, lapdir, lineage, &br, err, errsz))
        return false;
    if (br.type != REC_BRANCH || strcmp(br.id, lineage) != 0) {
        snprintf(err, errsz,
                 "the first record of branch %s is not its branch record",
                 lineage);
        return false;
    }
    Hist par, own;
    if (!open_view(a, lapdir, br.parent, br.base_chunk, depth + 1, &par, err,
                   errsz)) {
        if (depth > 0)
            return false; /* the outermost branch says it, once */
        char why[512];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, errsz, "branch %s starts after its base, but %s",
                 br.name, why);
        return false;
    }
    if (!hist_open(a, lapdir, lineage, &own, err, errsz))
        return false;
    if (upto > 0 && own.n < upto) {
        char name[64];
        hist_chunk_name(lineage, upto, name);
        snprintf(err, errsz, "%s is missing from %s", name, own.dir);
        return false;
    }
    if (upto > 0)
        own.n = upto;
    for (int32_t i = 0; i < own.n; i++)
        own.v[i].label = br.name;
    *h = own;
    h->cap = par.n + own.n;
    h->v = (HistChunk *)arena_alloc(a, (size_t)h->cap * sizeof(HistChunk));
    memcpy(h->v, par.v, (size_t)par.n * sizeof(HistChunk));
    memcpy(h->v + par.n, own.v, (size_t)own.n * sizeof(HistChunk));
    h->n = h->cap;
    uint64_t start = 0;
    for (int32_t i = 0; i < h->n; i++) {
        h->v[i].start = start;
        start += h->v[i].size;
    }
    h->size = start;
    snprintf(h->parent, sizeof h->parent, "%s", br.parent);
    snprintf(h->base, sizeof h->base, "%s", br.base);
    h->base_chunk = br.base_chunk;
    snprintf(h->name, sizeof h->name, "%s", br.name);
    return true;
}

bool hist_open_view(Arena *a, const char *lapdir, const char *lineage,
                    int32_t upto, Hist *h, char *err, size_t errsz) {
    return open_view(a, lapdir, lineage, upto, 0, h, err, errsz);
}

bool hist_open_lineage(Arena *a, const char *lapdir, const char *lineage,
                       Hist *h, char *err, size_t errsz) {
    return open_view(a, lapdir, lineage, 0, 0, h, err, errsz);
}

typedef struct {
    Arena *a;
    const char **v;
    int32_t n, cap;
} LineageList;

static WalkAction on_first_chunk(const char *rel, bool is_dir,
                                 const PlatStat *st, void *ud) {
    (void)st;
    LineageList *l = (LineageList *)ud;
    char lineage[HIST_LINEAGE_MAX];
    int32_t n;
    if (is_dir)
        return WALK_SKIP_DIR;
    if (!hist_parse_name(rel, lineage, &n) || n != 1 ||
        strcmp(lineage, LAP_MAIN_LINEAGE) == 0)
        return WALK_CONT;
    ARENA_GROW(l->a, l->v, l->n, l->cap, const char *);
    l->v[l->n++] = arena_strdup(l->a, lineage);
    return WALK_CONT;
}

int32_t hist_lineages(Arena *a, const char *lapdir, const char ***out) {
    char dir[LAP_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", lapdir, LAP_LOG_DIR);
    LineageList l = {a, NULL, 0, 0};
    if (plat_is_dir(dir))
        plat_walk(a, dir, on_first_chunk, &l);
    *out = l.v;
    return l.n;
}

void hist_name_break(const Hist *h, const char *data, RecLog *log) {
    /* A record whose prev does not match was usually preceded by a
     * changed record: the chunk holding that one is to blame, which for
     * a chunk's first record is the chunk before. A sealed chunk to
     * blame changed by mistake (a bad conflict resolution, a
     * repository-wide replace): lap never writes one. */
    int32_t at = hist_locate(h, log->chain_break_off);
    if (at > 0 && h->v[at].start == log->chain_break_off) {
        /* At a chunk's first record the boundary alone cannot say which
         * side changed; where the record's prev points can. */
        int32_t n = at;
        do
            at--;
        while (at > 0 && h->v[at].size == 0);
        const char *prev = log->v[log->chain_break_index].prev;
        int32_t j = 0;
        while (j < log->count && strcmp(log->v[j].hash, prev) != 0)
            j++;
        int32_t cj = j < log->count
                         ? hist_locate(h, (uint64_t)(log->v[j].raw - data))
                         : -1;
        bool last = j + 1 >= log->count ||
                    hist_locate(h, (uint64_t)(log->v[j + 1].raw - data)) !=
                        cj;
        char was[256];
        snprintf(was, sizeof was, "%s", log->chain_err);
        if (cj >= 0 && cj != at && last) {
            /* a stray or duplicated chunk: it continues another one */
            snprintf(log->chain_err, sizeof log->chain_err,
                     "chunk %s does not belong after %s: it continues "
                     "%s (%s)",
                     h->v[n].name, h->v[at].name, h->v[cj].name, was);
            return;
        }
        if (cj != at) { /* no clue which side changed: name both */
            snprintf(log->chain_err, sizeof log->chain_err,
                     "chunk %s does not continue %s%s (%s)", h->v[n].name,
                     hist_is_sealed(h, at) ? "sealed chunk " : "",
                     h->v[at].name, was);
            return;
        }
        /* it continues a record inside the chunk before, which was
         * appended to after it was sealed: that one is to blame */
    }
    int32_t sealed = hist_is_sealed(h, at) ? at : -1;
    if (sealed >= 0) {
        char was[256];
        snprintf(was, sizeof was, "%s", log->chain_err);
        snprintf(log->chain_err, sizeof log->chain_err,
                 "sealed chunk %s was modified: %s", h->v[sealed].name,
                 was);
    }
}

bool hist_scan(Arena *a, const Hist *h, HistScan *out, char *err,
               size_t errsz) {
    memset(out, 0, sizeof *out);
    out->chain_ok = true;
    snprintf(out->last_hash, sizeof out->last_hash, "%s", LAP_HASH_ZERO);
    Arena *ca = arena_new(1 << 16), *ra = arena_new(1 << 16);
    bool ok = true;
    for (int32_t k = 0; ok && k < h->n; k++) {
        if (h->v[k].size == 0)
            continue;
        arena_reset(ca);
        char *data;
        size_t len = (size_t)h->v[k].size;
        if (!hist_read(ca, h, h->v[k].start, len, &data)) {
            snprintf(err, errsz, "cannot read %s", h->v[k].name);
            ok = false;
            break;
        }
        for (size_t start = 0; start < len;) {
            const char *nl = memchr(data + start, '\n', len - start);
            if (!nl) { /* only the open chunk can end mid-line */
                out->torn_bytes = len - start;
                break;
            }
            size_t n = (size_t)(nl - (data + start));
            if (n > 0) {
                arena_reset(ra);
                Rec rec;
                char derr[128];
                if (!rec_decode(ra, data + start, n, &rec, derr,
                                sizeof derr)) {
                    snprintf(err, errsz, "%s: %s", h->v[k].name, derr);
                    ok = false;
                    break;
                }
                if (strcmp(rec.prev, out->last_hash) != 0)
                    out->chain_ok = false;
                if (rec.type == REC_UNKNOWN && out->unknown_n++ == 0)
                    out->unknown_type = arena_strdup(a, rec.name);
                snprintf(out->last_hash, sizeof out->last_hash, "%s",
                         rec.hash);
                out->records++;
            }
            start += n + 1;
        }
    }
    arena_free(ca);
    arena_free(ra);
    return ok;
}
