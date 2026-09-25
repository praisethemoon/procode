#include "gguf.h"

#include "quant.h"

/* Bounds on what will be believed before any of it is used as a length. A
 * corrupt or hostile file gets to say "four billion tensors"; it does not
 * get to have that number multiplied by a record size. */
#define GGUF_MAX_KV 65536u
#define GGUF_MAX_TENSORS 65536u
#define GGUF_MAX_STRING (16u * 1024u * 1024u)
#define GGUF_MAX_ARRAY (64u * 1024u * 1024u)

/* ---- a bounds-checked cursor -------------------------------------------
 *
 * Every read goes through this. `bad` latches on the first overrun and is
 * never cleared, so a caller may issue a run of reads and check once at the
 * end: after a failure every subsequent read returns zero rather than
 * walking off the mapping. */
typedef struct {
    const uint8_t *base;
    size_t len;
    size_t off;
    bool bad;
} Cur;

static bool cur_take(Cur *c, size_t n, const uint8_t **out) {
    if (c->bad || n > c->len - c->off) {
        c->bad = true;
        return false;
    }
    *out = c->base + c->off;
    c->off += n;
    return true;
}

static uint8_t cur_u8(Cur *c) {
    const uint8_t *p;
    return cur_take(c, 1, &p) ? p[0] : 0;
}

static uint16_t cur_u16(Cur *c) {
    const uint8_t *p;
    if (!cur_take(c, 2, &p))
        return 0;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t cur_u32(Cur *c) {
    const uint8_t *p;
    if (!cur_take(c, 4, &p))
        return 0;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t cur_u64(Cur *c) {
    uint64_t lo = cur_u32(c);
    uint64_t hi = cur_u32(c);
    return lo | (hi << 32);
}

static float cur_f32(Cur *c) {
    uint32_t bits = cur_u32(c);
    float f;
    /* Through memcpy rather than a union or a cast: the bytes are an IEEE-754
     * single in the file and this is the one spelling that is not an aliasing
     * violation. */
    memcpy(&f, &bits, sizeof f);
    return f;
}

static double cur_f64(Cur *c) {
    uint64_t bits = cur_u64(c);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

/* A GGUF string is a length and then unterminated bytes. The view points
 * into the mapping; nothing is copied. */
static bool cur_str(Cur *c, Str *out) {
    uint64_t n = cur_u64(c);
    if (c->bad || n > GGUF_MAX_STRING) {
        c->bad = true;
        return false;
    }
    const uint8_t *p;
    if (!cur_take(c, (size_t)n, &p))
        return false;
    out->ptr = (const char *)p;
    out->len = (size_t)n;
    return true;
}

/* ---- element widths ----------------------------------------------------- */

/* Fixed size of one metadata value, or 0 for STRING and ARRAY, which are
 * variable and are walked rather than indexed. */
static size_t kv_elem_size(GgufType t) {
    switch (t) {
    case GGUF_U8:
    case GGUF_I8:
    case GGUF_BOOL:
        return 1;
    case GGUF_U16:
    case GGUF_I16:
        return 2;
    case GGUF_U32:
    case GGUF_I32:
    case GGUF_F32:
        return 4;
    case GGUF_U64:
    case GGUF_I64:
    case GGUF_F64:
        return 8;
    default:
        return 0;
    }
}

/* Reads one scalar into all three numeric shapes. A caller asking for a
 * uint64 from a file that stored an int32 gets the value, not the encoding. */
static void read_scalar(Cur *c, GgufType t, GgufKV *kv) {
    switch (t) {
    case GGUF_U8:
        kv->u = cur_u8(c);
        kv->i = (int64_t)kv->u;
        break;
    case GGUF_I8:
        kv->i = (int8_t)cur_u8(c);
        kv->u = (uint64_t)kv->i;
        break;
    case GGUF_U16:
        kv->u = cur_u16(c);
        kv->i = (int64_t)kv->u;
        break;
    case GGUF_I16:
        kv->i = (int16_t)cur_u16(c);
        kv->u = (uint64_t)kv->i;
        break;
    case GGUF_U32:
        kv->u = cur_u32(c);
        kv->i = (int64_t)kv->u;
        break;
    case GGUF_I32:
        kv->i = (int32_t)cur_u32(c);
        kv->u = (uint64_t)kv->i;
        break;
    case GGUF_U64:
        kv->u = cur_u64(c);
        kv->i = (int64_t)kv->u;
        break;
    case GGUF_I64:
        kv->i = (int64_t)cur_u64(c);
        kv->u = (uint64_t)kv->i;
        break;
    case GGUF_BOOL:
        kv->u = cur_u8(c) != 0;
        kv->i = (int64_t)kv->u;
        break;
    case GGUF_F32:
        kv->f = (double)cur_f32(c);
        kv->i = (int64_t)kv->f;
        kv->u = (uint64_t)kv->i;
        return;
    case GGUF_F64:
        kv->f = cur_f64(c);
        kv->i = (int64_t)kv->f;
        kv->u = (uint64_t)kv->i;
        return;
    default:
        c->bad = true;
        return;
    }
    kv->f = (double)kv->i;
}

const char *gguf_type_name(uint32_t t) {
    switch (t) {
    case GGML_F32:
        return "F32";
    case GGML_F16:
        return "F16";
    case GGML_Q4_K:
        return "Q4_K";
    case GGML_Q5_K:
        return "Q5_K";
    case GGML_Q6_K:
        return "Q6_K";
    default:
        return "unsupported";
    }
}

/* ---- open --------------------------------------------------------------- */

static bool fail(Gguf *g, char *err, size_t errsz, const char *path,
                 const char *what) {
    snprintf(err, errsz, "%s: %s", path, what);
    gguf_close(g);
    return false;
}

bool gguf_open(Arena *a, const char *path, Gguf *out, char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    out->map = plat_map_file(a, path, &out->base, &out->len);
    if (!out->map) {
        snprintf(err, errsz, "%s: cannot be opened or is empty", path);
        return false;
    }

    Cur c;
    c.base = out->base;
    c.len = out->len;
    c.off = 0;
    c.bad = false;

    const uint8_t *magic;
    if (!cur_take(&c, 4, &magic))
        return fail(out, err, errsz, path, "too short to hold a GGUF header");
    if (memcmp(magic, "GGUF", 4) != 0)
        return fail(out, err, errsz, path,
                    "not a GGUF file (the first four bytes are not \"GGUF\")");
    out->version = cur_u32(&c);
    /* Version 1 stored tensor counts as 32-bit and laid the directory out
     * differently. Refusing it by name is better service than reading it as
     * if it were version 2 and producing plausible nonsense. */
    if (out->version != 2 && out->version != 3) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "GGUF version %lu; this build reads versions 2 and 3",
                 (unsigned long)out->version);
        return fail(out, err, errsz, path, msg);
    }
    out->n_tensors = cur_u64(&c);
    out->n_kv = cur_u64(&c);
    if (c.bad)
        return fail(out, err, errsz, path, "header is truncated");
    if (out->n_tensors > GGUF_MAX_TENSORS || out->n_kv > GGUF_MAX_KV)
        return fail(out, err, errsz, path,
                    "header claims more tensors or metadata keys than any "
                    "real model has");

    out->kv = (GgufKV *)arena_alloc0(
        a, (size_t)(out->n_kv ? out->n_kv : 1) * sizeof(GgufKV));
    for (uint64_t i = 0; i < out->n_kv; i++) {
        GgufKV *kv = &out->kv[i];
        Str key;
        if (!cur_str(&c, &key))
            return fail(out, err, errsz, path, "metadata key is truncated");
        kv->key = str_dup_c(a, key);
        kv->type = (GgufType)cur_u32(&c);
        kv->count = 1;
        if (kv->type == GGUF_ARRAY) {
            kv->elem = (GgufType)cur_u32(&c);
            kv->count = cur_u64(&c);
            if (c.bad)
                return fail(out, err, errsz, path,
                            "metadata array header is truncated");
            if (kv->elem >= GGUF_TYPE_COUNT || kv->elem == GGUF_ARRAY)
                return fail(out, err, errsz, path,
                            "metadata array has an element type this build "
                            "does not know");
            if (kv->count > GGUF_MAX_ARRAY)
                return fail(out, err, errsz, path,
                            "metadata array is longer than any real model's");
            size_t off0 = c.off;
            if (kv->elem == GGUF_STRING) {
                /* Walked rather than skipped: the only way to know where a
                 * run of length-prefixed strings ends is to read every
                 * length, and reading them here is what proves they fit. */
                for (uint64_t k = 0; k < kv->count; k++) {
                    Str s;
                    if (!cur_str(&c, &s))
                        return fail(out, err, errsz, path,
                                    "metadata string array is truncated");
                }
            } else {
                size_t w = kv_elem_size(kv->elem);
                const uint8_t *p;
                if (w == 0 || kv->count > (uint64_t)(SIZE_MAX / w) ||
                    !cur_take(&c, (size_t)(kv->count * w), &p))
                    return fail(out, err, errsz, path,
                                "metadata array is truncated");
            }
            kv->payload = out->base + off0;
            kv->payload_len = c.off - off0;
            continue;
        }
        if (kv->type >= GGUF_TYPE_COUNT)
            return fail(out, err, errsz, path,
                        "metadata value has a type this build does not know");
        size_t off0 = c.off;
        if (kv->type == GGUF_STRING) {
            Str s;
            if (!cur_str(&c, &s))
                return fail(out, err, errsz, path,
                            "metadata string is truncated");
            kv->s = str_dup_c(a, s);
        } else {
            read_scalar(&c, kv->type, kv);
            if (c.bad)
                return fail(out, err, errsz, path,
                            "metadata value is truncated");
        }
        kv->payload = out->base + off0;
        kv->payload_len = c.off - off0;
    }

    out->tensors = (GgufTensor *)arena_alloc0(
        a, (size_t)(out->n_tensors ? out->n_tensors : 1) * sizeof(GgufTensor));
    for (uint64_t i = 0; i < out->n_tensors; i++) {
        GgufTensor *t = &out->tensors[i];
        Str name;
        if (!cur_str(&c, &name))
            return fail(out, err, errsz, path, "tensor name is truncated");
        t->name = str_dup_c(a, name);
        t->n_dims = cur_u32(&c);
        if (c.bad || t->n_dims == 0 || t->n_dims > GGUF_MAX_DIMS)
            return fail(out, err, errsz, path,
                        "tensor has no dimensions or more than four");
        t->ne[0] = t->ne[1] = t->ne[2] = t->ne[3] = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) {
            t->ne[d] = cur_u64(&c);
            if (c.bad || t->ne[d] == 0)
                return fail(out, err, errsz, path,
                            "tensor has a zero or truncated dimension");
        }
        t->type = cur_u32(&c);
        t->offset = cur_u64(&c);
        if (c.bad)
            return fail(out, err, errsz, path, "tensor record is truncated");
    }

    /* The alignment is metadata, so it is read only after the metadata has
     * been. 32 is the format's default. A non-power-of-two would make the
     * rounding below meaningless. */
    out->alignment = gguf_u64(out, "general.alignment", 32);
    if (out->alignment == 0 || out->alignment > 65536 ||
        (out->alignment & (out->alignment - 1)) != 0)
        return fail(out, err, errsz, path,
                    "general.alignment is not a power of two");
    uint64_t dir_end = (uint64_t)c.off;
    out->data_off = (dir_end + out->alignment - 1) & ~(out->alignment - 1);
    if (out->data_off > out->len)
        return fail(out, err, errsz, path,
                    "the tensor data section starts past the end of the file");

    uint64_t avail = (uint64_t)out->len - out->data_off;
    for (uint64_t i = 0; i < out->n_tensors; i++) {
        GgufTensor *t = &out->tensors[i];
        uint64_t elems = 1;
        for (uint32_t d = 0; d < GGUF_MAX_DIMS; d++) {
            if (t->ne[d] && elems > UINT64_MAX / t->ne[d])
                return fail(out, err, errsz, path,
                            "a tensor's dimensions multiply out past 2^64");
            elems *= t->ne[d];
        }
        uint64_t row_bytes;
        if (!quant_row_bytes(t->type, t->ne[0], &row_bytes)) {
            char msg[192];
            snprintf(msg, sizeof msg,
                     "tensor \"%s\" is %s, which this build cannot read, or "
                     "its row length %llu is not a whole number of blocks",
                     t->name, gguf_type_name(t->type),
                     (unsigned long long)t->ne[0]);
            return fail(out, err, errsz, path, msg);
        }
        uint64_t rows = elems / t->ne[0];
        if (rows && row_bytes > UINT64_MAX / rows)
            return fail(out, err, errsz, path,
                        "a tensor's byte size overflows");
        t->bytes = row_bytes * rows;
        /* Both halves matter: an offset inside the file whose length runs
         * past the end is exactly the read this check exists to stop. */
        if (t->offset > avail || t->bytes > avail - t->offset) {
            char msg[192];
            snprintf(msg, sizeof msg,
                     "tensor \"%s\" claims %llu bytes at offset %llu, past "
                     "the end of the file",
                     t->name, (unsigned long long)t->bytes,
                     (unsigned long long)t->offset);
            return fail(out, err, errsz, path, msg);
        }
        t->data = out->base + out->data_off + t->offset;
    }
    return true;
}

void gguf_close(Gguf *g) {
    if (g->map) {
        plat_unmap_file(g->map);
        g->map = NULL;
    }
    g->base = NULL;
    g->len = 0;
}

/* ---- lookups ------------------------------------------------------------ */

const GgufKV *gguf_find(const Gguf *g, const char *key) {
    for (uint64_t i = 0; i < g->n_kv; i++) {
        if (strcmp(g->kv[i].key, key) == 0)
            return &g->kv[i];
    }
    return NULL;
}

uint64_t gguf_u64(const Gguf *g, const char *key, uint64_t def) {
    const GgufKV *kv = gguf_find(g, key);
    if (!kv || kv->type == GGUF_ARRAY || kv->type == GGUF_STRING)
        return def;
    return kv->u;
}

double gguf_f64(const Gguf *g, const char *key, double def) {
    const GgufKV *kv = gguf_find(g, key);
    if (!kv || kv->type == GGUF_ARRAY || kv->type == GGUF_STRING)
        return def;
    return kv->f;
}

const char *gguf_str(const Gguf *g, const char *key, const char *def) {
    const GgufKV *kv = gguf_find(g, key);
    return kv && kv->type == GGUF_STRING ? kv->s : def;
}

bool gguf_bool(const Gguf *g, const char *key, bool def) {
    const GgufKV *kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_BOOL)
        return def;
    return kv->u != 0;
}

bool gguf_str_array(Arena *a, const Gguf *g, const char *key, Str **out,
                    uint64_t *n) {
    *out = NULL;
    *n = 0;
    const GgufKV *kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_ARRAY || kv->elem != GGUF_STRING)
        return false;
    /* The payload was already walked whole at open time, so this second
     * pass cannot overrun; the cursor is kept anyway because a reader that
     * trusts a previous pass is one edit away from not being able to. */
    Cur c;
    c.base = kv->payload;
    c.len = kv->payload_len;
    c.off = 0;
    c.bad = false;
    Str *v = (Str *)arena_alloc(a, (size_t)(kv->count ? kv->count : 1) *
                                       sizeof(Str));
    for (uint64_t i = 0; i < kv->count; i++) {
        if (!cur_str(&c, &v[i]))
            return false;
    }
    *out = v;
    *n = kv->count;
    return true;
}

bool gguf_i32_array(Arena *a, const Gguf *g, const char *key, int32_t **out,
                    uint64_t *n) {
    *out = NULL;
    *n = 0;
    const GgufKV *kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_ARRAY ||
        (kv->elem != GGUF_I32 && kv->elem != GGUF_U32))
        return false;
    Cur c;
    c.base = kv->payload;
    c.len = kv->payload_len;
    c.off = 0;
    c.bad = false;
    int32_t *v = (int32_t *)arena_alloc(
        a, (size_t)(kv->count ? kv->count : 1) * sizeof(int32_t));
    for (uint64_t i = 0; i < kv->count; i++)
        v[i] = (int32_t)cur_u32(&c);
    if (c.bad)
        return false;
    *out = v;
    *n = kv->count;
    return true;
}

const GgufTensor *gguf_tensor(const Gguf *g, const char *name) {
    for (uint64_t i = 0; i < g->n_tensors; i++) {
        if (strcmp(g->tensors[i].name, name) == 0)
            return &g->tensors[i];
    }
    return NULL;
}

const GgufTensor *gguf_layer_tensor(const Gguf *g, uint32_t layer,
                                    const char *suffix) {
    char name[256];
    snprintf(name, sizeof name, "blk.%lu.%s", (unsigned long)layer, suffix);
    return gguf_tensor(g, name);
}
