#include "vectors.h"

#include "cmd.h"
#include "modelrec.h"
#include "platform.h"

#include <math.h>
#include <string.h>

#define VEC_MAGIC "KBVEC001"
#define VEC_HEADER 88u /* magic 8, dim 4, reserved 4, count 8, fingerprint 64 */

static void vec_path(const Store *s, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", s->dir, KB_VEC_NAME);
}

static size_t record_bytes(uint32_t dim) {
    return 8 + 4 + (size_t)dim;
}

void vec_load(Arena *a, const Store *s, VecSet *out) {
    memset(out, 0, sizeof *out);
    char path[KB_PATH_MAX];
    vec_path(s, path, sizeof path);
    char *data;
    size_t len;
    if (!plat_read_file_max(a, path, &data, &len, (size_t)-1) ||
        len < VEC_HEADER || memcmp(data, VEC_MAGIC, 8) != 0)
        return;
    uint32_t dim;
    uint64_t count;
    memcpy(&dim, data + 8, 4);
    memcpy(&count, data + 16, 8);
    if (dim == 0 || dim > 16384 ||
        count > (len - VEC_HEADER) / record_bytes(dim) ||
        len != VEC_HEADER + count * record_bytes(dim))
        return; /* not a file this build wrote: treated as absent */
    out->dim = dim;
    out->n = (size_t)count;
    memcpy(out->fingerprint, data + 24, 64);
    out->fingerprint[64] = '\0';
    out->ids = (int64_t *)arena_alloc(a, (out->n ? out->n : 1) * sizeof(int64_t));
    out->scales = (float *)arena_alloc(a, (out->n ? out->n : 1) * sizeof(float));
    out->q = (int8_t *)arena_alloc(a, (out->n ? out->n : 1) * dim);
    const char *p = data + VEC_HEADER;
    for (size_t i = 0; i < out->n; i++) {
        memcpy(&out->ids[i], p, 8);
        memcpy(&out->scales[i], p + 8, 4);
        memcpy(out->q + i * dim, p + 12, dim);
        p += record_bytes(dim);
    }
}

bool vec_save(Arena *a, const Store *s, const VecSet *v, char *err,
              size_t errsz) {
    size_t len = VEC_HEADER + v->n * record_bytes(v->dim);
    char *buf = (char *)arena_alloc0(a, len);
    memcpy(buf, VEC_MAGIC, 8);
    memcpy(buf + 8, &v->dim, 4);
    uint64_t count = v->n;
    memcpy(buf + 16, &count, 8);
    memcpy(buf + 24, v->fingerprint, 64);
    char *p = buf + VEC_HEADER;
    for (size_t i = 0; i < v->n; i++) {
        memcpy(p, &v->ids[i], 8);
        memcpy(p + 8, &v->scales[i], 4);
        memcpy(p + 12, v->q + i * v->dim, v->dim);
        p += record_bytes(v->dim);
    }
    char path[KB_PATH_MAX];
    vec_path(s, path, sizeof path);
    if (!plat_mkdirs(s->index_dir) || !plat_write_file_atomic(path, buf, len)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    return true;
}

int64_t vec_find(const VecSet *v, int64_t id) {
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v->ids[mid] < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < v->n && v->ids[lo] == id) ? (int64_t)lo : -1;
}

float vec_score(const VecSet *v, size_t i, const float *x) {
    const int8_t *q = v->q + i * v->dim;
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    uint32_t d = 0;
    for (; d + 4 <= v->dim; d += 4) {
        s0 += (float)q[d] * x[d];
        s1 += (float)q[d + 1] * x[d + 1];
        s2 += (float)q[d + 2] * x[d + 2];
        s3 += (float)q[d + 3] * x[d + 3];
    }
    for (; d < v->dim; d++)
        s0 += (float)q[d] * x[d];
    return v->scales[i] * ((s0 + s1) + (s2 + s3));
}

size_t vec_missing(const Store *s, const VecSet *v) {
    size_t missing = 0;
    for (size_t i = 0; i < s->documents.n; i++) {
        const Document *d = &s->documents.v[i];
        for (uint32_t j = 0; j < d->chunk_count; j++)
            if (vec_find(v, d->chunk_base + (int64_t)j) < 0)
                missing++;
    }
    return missing;
}

static void quantise(const float *x, uint32_t dim, int8_t *q, float *scale) {
    float max = 0.0f;
    for (uint32_t i = 0; i < dim; i++)
        if (fabsf(x[i]) > max)
            max = fabsf(x[i]);
    *scale = max > 0.0f ? max / 127.0f : 1.0f;
    for (uint32_t i = 0; i < dim; i++) {
        float r = roundf(x[i] / *scale);
        q[i] = (int8_t)(r > 127.0f ? 127 : (r < -127.0f ? -127 : r));
    }
}

bool vec_sync(Arena *a, Store *s, Embedder *e, bool all, bool progress,
              VecSync *stats, char *err, size_t errsz) {
    memset(stats, 0, sizeof *stats);
    if (!s->lock) {
        snprintf(err, errsz, "internal: vectors written without the lock");
        return false;
    }
    char fp[65];
    model_fingerprint(&e->cfg, fp);
    VecSet old;
    vec_load(a, s, &old);
    if (all || strcmp(old.fingerprint, fp) != 0 || old.dim != e->n_embd)
        old.n = 0; /* written under something else: none of it carries over */

    size_t live = 0;
    for (size_t i = 0; i < s->documents.n; i++)
        live += s->documents.v[i].chunk_count;
    VecSet next;
    memset(&next, 0, sizeof next);
    next.dim = e->n_embd;
    memcpy(next.fingerprint, fp, 65);
    next.ids = (int64_t *)arena_alloc(a, (live ? live : 1) * sizeof(int64_t));
    next.scales = (float *)arena_alloc(a, (live ? live : 1) * sizeof(float));
    next.q = (int8_t *)arena_alloc(a, (live ? live : 1) * next.dim);
    float *vec = (float *)arena_alloc(a, next.dim * sizeof(float));

    size_t todo = live, done = 0;
    for (size_t i = 0; i < s->documents.n; i++) {
        const Document *d = &s->documents.v[i];
        if (d->chunk_count == 0)
            continue;
        /* The document's text and chunks, read only when one of its chunks
         * needs embedding, into an arena of its own so a rebuild of a large
         * store does not hold every blob at once. */
        Arena *tmp = NULL;
        char *text = NULL;
        size_t len = 0;
        Chunks ch;
        memset(&ch, 0, sizeof ch);
        bool have_text = false;
        for (uint32_t j = 0; j < d->chunk_count; j++) {
            int64_t id = d->chunk_base + (int64_t)j;
            int64_t at = vec_find(&old, id);
            size_t k = next.n;
            if (at >= 0) {
                next.ids[k] = id;
                next.scales[k] = old.scales[at];
                memcpy(next.q + k * next.dim, old.q + (size_t)at * old.dim,
                       next.dim);
                next.n++;
                stats->kept++;
                done++;
                continue;
            }
            if (!have_text) {
                tmp = arena_new(1 << 20);
                have_text = doc_chunks(tmp, s, d, &text, &len, &ch);
                if (!have_text)
                    break; /* no blob: its chunks stay without vectors */
            }
            if (j >= ch.n)
                break; /* the log's range and the text disagree: reindex */
            bool truncated = false;
            /* A code chunk is embedded under its header line: which file,
             * which function (chunk_header). */
            const char *body = text + ch.v[j].start;
            size_t blen = ch.v[j].end - ch.v[j].start;
            const char *header = chunk_header(tmp, doc_lang(d->mime, d->path),
                                              d->title, &ch.v[j]);
            if (header) {
                char *joined = arena_printf(tmp, "%s\n%.*s", header, (int)blen, body);
                body = joined;
                blen = strlen(joined);
            }
            if (!embed_text(e, body, blen, false, vec, &truncated)) {
                if (tmp)
                    arena_free(tmp);
                snprintf(err, errsz, "embedding C-%lld failed", (long long)id);
                return false;
            }
            if (truncated)
                stats->truncated++;
            next.ids[k] = id;
            quantise(vec, next.dim, next.q + k * next.dim, &next.scales[k]);
            next.n++;
            stats->embedded++;
            done++;
            if (progress)
                fprintf(stderr, "\rembedding %zu/%zu chunks", done, todo);
        }
        if (tmp)
            arena_free(tmp);
    }
    if (progress && stats->embedded)
        fputc('\n', stderr);
    /* Documents are in log order and each one's chunks ascend, but ranges
     * from different documents interleave after a re-file; the file is kept
     * sorted so lookups can bisect it. Insertion sort over records that are
     * nearly in order already. */
    for (size_t i = 1; i < next.n; i++) {
        int64_t id = next.ids[i];
        float sc = next.scales[i];
        size_t j = i;
        if (next.ids[j - 1] <= id)
            continue;
        int8_t *row = (int8_t *)arena_alloc(a, next.dim);
        memcpy(row, next.q + i * next.dim, next.dim);
        while (j > 0 && next.ids[j - 1] > id) {
            next.ids[j] = next.ids[j - 1];
            next.scales[j] = next.scales[j - 1];
            memcpy(next.q + j * next.dim, next.q + (j - 1) * next.dim,
                   next.dim);
            j--;
        }
        next.ids[j] = id;
        next.scales[j] = sc;
        memcpy(next.q + j * next.dim, row, next.dim);
    }
    stats->dropped = old.n > stats->kept ? old.n - stats->kept : 0;
    return vec_save(a, s, &next, err, errsz);
}

bool vec_update(Arena *a, Store *s, bool all, bool progress, VecSync *stats,
                bool *ran, char *err, size_t errsz) {
    memset(stats, 0, sizeof *stats);
    *ran = false;
    ModelParams recorded;
    char sha[65];
    if (!model_recorded(a, s, &recorded, sha))
        return true;
    ModelProbe probe;
    model_probe(a, &probe);
    if (!probe.found || !model_params_equal(&recorded, &probe.params))
        return true;
    Embedder e;
    char why[512];
    if (!embed_open(a, probe.path, &e, why, sizeof why)) {
        snprintf(err, errsz, "%s", why);
        return false;
    }
    bool ok = vec_sync(a, s, &e, all, progress, stats, err, errsz);
    embed_close(&e);
    *ran = ok;
    return ok;
}
