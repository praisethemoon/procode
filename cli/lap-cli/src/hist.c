#include "hist.h"

#include "rec.h"
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

bool hist_open(Arena *a, const char *lapdir, const char *lineage, Hist *h,
               char *err, size_t errsz) {
    memset(h, 0, sizeof *h);
    snprintf(h->dir, sizeof h->dir, "%s/%s", lapdir, LAP_LOG_DIR);
    snprintf(h->lineage, sizeof h->lineage, "%s", lineage);
    h->limit = hist_chunk_limit();
    if (!plat_is_dir(h->dir))
        return true;
    ListCtx c = {a, h, lineage};
    if (!plat_walk(a, h->dir, on_log_entry, &c)) {
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

void hist_chunk_path(const Hist *h, int32_t i, char *out, size_t outsz) {
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
        char *part;
        if (!plat_read_range(a, path, in, take, &part))
            return false;
        memcpy(buf + got, part, take);
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
    if (!plat_append_file_sync(path, "", 0)) {
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
