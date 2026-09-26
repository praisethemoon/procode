#include "embed.h"

#include "quant.h"
#include "sha256.h"
#include "store.h"

#include <math.h>

/* ---- finding the weights ------------------------------------------------ */

/* The one model kb knows how to get, and how to get it. kb never downloads
 * anything (§12.2: no HTTP client in the binary); the user runs this. */
#define KB_MODEL_URL                                                           \
    "https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/" \
    "nomic-embed-text-v1.5.Q4_K_M.gguf"
#define KB_MODEL_FILE "nomic-embed-text-v1.5.Q4_K_M.gguf"

typedef struct {
    Arena *a;
    const char *dir;
    char *found;
    uint32_t n;
} GgufScan;

static WalkAction gguf_visit(const char *rel, bool is_dir, void *ud) {
    GgufScan *s = (GgufScan *)ud;
    if (is_dir)
        return WALK_SKIP_DIR; /* models/ is flat; a subdirectory is not ours */
    size_t n = strlen(rel);
    if (n < 6 || strcmp(rel + n - 5, ".gguf") != 0)
        return WALK_CONT;
    s->n++;
    if (!s->found)
        s->found = arena_printf(s->a, "%s/%s", s->dir, rel);
    return WALK_CONT;
}

/* ~/.kb/models: the machine's models, shared by every workspace and only ever
 * READ. kb creates nothing there and deletes nothing there — the user puts
 * the file in place with the command the error prints. There is no store
 * under ~/.kb; this directory is the only thing kb looks at under the home
 * directory, and which model a workspace was indexed with is still pinned in
 * its own index/model.json. The model must be a regular file: like every
 * walk in kb, this one skips symbolic links. */
bool embed_models_dir(char *out, size_t outsz) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0])
        home = getenv("USERPROFILE");
#endif
    if (!home || !home[0])
        return false;
    if (snprintf(out, outsz, "%s/.kb/models", home) >= (int32_t)outsz)
        return false;
    for (char *p = out; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    return true;
}

bool embed_find_model(Arena *a, char *out, size_t outsz, char *err,
                      size_t errsz) {
    char models[KB_PATH_MAX];
    if (!embed_models_dir(models, sizeof models)) {
        snprintf(err, errsz, "no embedding model: HOME is not set, so there "
                             "is no ~/.kb/models to look in");
        return false;
    }
    GgufScan s;
    memset(&s, 0, sizeof s);
    s.a = a;
    s.dir = models;
    if (plat_is_dir(models))
        plat_walk(a, models, gguf_visit, &s);
    if (!s.found) {
        snprintf(err, errsz,
                 "no embedding model in %s; download it with:\n"
                 "  mkdir -p %s && curl -fL -o %s/" KB_MODEL_FILE " \\\n"
                 "    " KB_MODEL_URL,
                 models, models, models);
        return false;
    }
    /* More than one is ambiguous, and picking the alphabetically first would
     * make which model a store was built with depend on a directory listing. */
    if (s.n > 1) {
        snprintf(err, errsz,
                 "%lu .gguf files in %s; keep exactly one", (unsigned long)s.n,
                 models);
        return false;
    }
    if (snprintf(out, outsz, "%s", s.found) >= (int32_t)outsz) {
        snprintf(err, errsz, "the model path is longer than a path may be");
        return false;
    }
    return true;
}

/* ---- the asymmetric prefixes (§8) ---------------------------------------
 *
 * The GGUF does not carry them — they are a property of how the model was
 * trained, not of how it was serialised — so they come from its identity.
 * An unrecognised model gets no prefixes and index/model.json says so, which
 * is honest: empty prefixes are what a symmetric model wants, and a reader
 * can see that kb did not recognise theirs.
 */
static void default_prefixes(const char *arch, const char *name,
                             const char **q, const char **d) {
    *q = "";
    *d = "";
    if (strcmp(arch, "nomic-bert") == 0) {
        *q = "search_query: ";
        *d = "search_document: ";
        return;
    }
    if (strncmp(name, "bge-", 4) == 0) {
        *q = "Represent this sentence for searching relevant passages: ";
        return;
    }
    if (strstr(name, "e5-") != NULL) {
        *q = "query: ";
        *d = "passage: ";
    }
}

/* ---- opening ------------------------------------------------------------ */

static const GgufTensor *need(const Gguf *g, const char *name, char *err,
                              size_t errsz, bool *ok) {
    const GgufTensor *t = gguf_tensor(g, name);
    if (!t) {
        if (*ok)
            snprintf(err, errsz, "the model file has no tensor \"%s\"", name);
        *ok = false;
    }
    return t;
}

static bool shape2(const GgufTensor *t, uint64_t ne0, uint64_t ne1,
                   const char *what, char *err, size_t errsz) {
    if (!t)
        return false;
    if (t->ne[0] != ne0 || t->ne[1] != ne1) {
        snprintf(err, errsz,
                 "%s is [%llu, %llu]; this architecture needs [%llu, %llu]",
                 what, (unsigned long long)t->ne[0],
                 (unsigned long long)t->ne[1], (unsigned long long)ne0,
                 (unsigned long long)ne1);
        return false;
    }
    return true;
}

/* The GGUF's file_type, which names the mix a quantiser produced. Recorded
 * in model.json because two quantisations of one model are two different
 * sets of vectors — a hole §8's example configuration leaves open, since it
 * names the model but not how its weights were rounded. */
static const char *file_type_name(uint64_t ft) {
    switch (ft) {
    case 0:
        return "F32";
    case 1:
        return "F16";
    case 7:
        return "Q8_0";
    case 12:
        return "Q3_K_M";
    case 14:
        return "Q4_K_S";
    case 15:
        return "Q4_K_M";
    case 16:
        return "Q5_K_S";
    case 17:
        return "Q5_K_M";
    case 18:
        return "Q6_K";
    default:
        return "unknown";
    }
}

bool embed_open(Arena *a, const char *path, Embedder *e, char *err,
                size_t errsz) {
    memset(e, 0, sizeof(*e));
    e->a = a;
    snprintf(e->path, sizeof e->path, "%s", path);
    if (!gguf_open(a, path, &e->g, err, errsz))
        return false;

    const char *arch = gguf_str(&e->g, "general.architecture", NULL);
    if (!arch) {
        snprintf(err, errsz, "%s: no general.architecture", path);
        embed_close(e);
        return false;
    }
    /* THE ONE ARCHITECTURE CHECK. Everything below reads shapes from the
     * file, but the ORDER of the operations — post-norm, fused QKV, rotary
     * positions, gated feed-forward — is written into the forward pass and
     * cannot be read from anywhere. So the name is checked, and a file
     * naming anything else is refused rather than run with this one's
     * wiring. */
    if (strcmp(arch, "nomic-bert") != 0) {
        snprintf(err, errsz,
                 "%s: architecture \"%s\"; this build runs \"nomic-bert\"",
                 path, arch);
        embed_close(e);
        return false;
    }

    char key[128];
#define ARCH_KEY(suffix)                                                       \
    (snprintf(key, sizeof key, "%s." suffix, arch), key)
    e->n_embd = (uint32_t)gguf_u64(&e->g, ARCH_KEY("embedding_length"), 0);
    e->n_layer = (uint32_t)gguf_u64(&e->g, ARCH_KEY("block_count"), 0);
    e->n_head = (uint32_t)gguf_u64(&e->g, ARCH_KEY("attention.head_count"), 0);
    e->n_ff = (uint32_t)gguf_u64(&e->g, ARCH_KEY("feed_forward_length"), 0);
    e->n_ctx = (uint32_t)gguf_u64(&e->g, ARCH_KEY("context_length"), 0);
    e->eps = (float)gguf_f64(&e->g, ARCH_KEY("attention.layer_norm_epsilon"),
                             1e-12);
    e->rope_base = (float)gguf_f64(&e->g, ARCH_KEY("rope.freq_base"), 10000.0);
    uint64_t pooling = gguf_u64(&e->g, ARCH_KEY("pooling_type"), 1);
    bool causal = gguf_bool(&e->g, ARCH_KEY("attention.causal"), false);
#undef ARCH_KEY

    if (!e->n_embd || !e->n_layer || !e->n_head || !e->n_ff ||
        e->n_embd % e->n_head != 0) {
        snprintf(err, errsz,
                 "%s: the metadata does not describe a usable shape "
                 "(embd=%lu layers=%lu heads=%lu ff=%lu)",
                 path, (unsigned long)e->n_embd, (unsigned long)e->n_layer,
                 (unsigned long)e->n_head, (unsigned long)e->n_ff);
        embed_close(e);
        return false;
    }
    if (causal) {
        /* A causal encoder pools over tokens that could not see each other's
         * successors. The forward pass below has no mask at all, so running
         * one here would be wrong in a way nothing downstream could notice. */
        snprintf(err, errsz,
                 "%s: the model is causal; this build runs non-causal "
                 "encoders only",
                 path);
        embed_close(e);
        return false;
    }
    if (pooling != 1 && pooling != 2) {
        snprintf(err, errsz,
                 "%s: pooling type %llu; this build pools by mean (1) or CLS "
                 "(2)",
                 path, (unsigned long long)pooling);
        embed_close(e);
        return false;
    }
    e->pool_mean = pooling == 1;
    e->n_head_dim = e->n_embd / e->n_head;
    /* RoPE rotates pairs across the two halves of a head, so an odd head
     * width has no pairing at all. */
    if (e->n_head_dim % 2 != 0) {
        snprintf(err, errsz, "%s: head width %lu is odd", path,
                 (unsigned long)e->n_head_dim);
        embed_close(e);
        return false;
    }

    if (!wpm_init(a, &e->g, &e->wpm, err, errsz)) {
        embed_close(e);
        return false;
    }

    bool ok = true;
    e->tok_embd = need(&e->g, "token_embd.weight", err, errsz, &ok);
    e->type_embd = need(&e->g, "token_types.weight", err, errsz, &ok);
    e->embd_norm_w = need(&e->g, "token_embd_norm.weight", err, errsz, &ok);
    e->embd_norm_b = need(&e->g, "token_embd_norm.bias", err, errsz, &ok);
    if (!ok) {
        embed_close(e);
        return false;
    }
    if (!shape2(e->tok_embd, e->n_embd, e->wpm.n, "token_embd.weight", err,
                errsz)) {
        embed_close(e);
        return false;
    }

    e->layer = (EmbedLayer *)arena_alloc0(a, e->n_layer * sizeof(EmbedLayer));
    for (uint32_t l = 0; l < e->n_layer; l++) {
        EmbedLayer *L = &e->layer[l];
        char n[128];
#define LAYER(field, suffix)                                                   \
    (snprintf(n, sizeof n, "blk.%lu." suffix, (unsigned long)l),               \
     L->field = need(&e->g, n, err, errsz, &ok))
        LAYER(qkv, "attn_qkv.weight");
        LAYER(attn_out, "attn_output.weight");
        LAYER(attn_norm_w, "attn_output_norm.weight");
        LAYER(attn_norm_b, "attn_output_norm.bias");
        LAYER(ffn_up, "ffn_up.weight");
        LAYER(ffn_gate, "ffn_gate.weight");
        LAYER(ffn_down, "ffn_down.weight");
        LAYER(out_norm_w, "layer_output_norm.weight");
        LAYER(out_norm_b, "layer_output_norm.bias");
#undef LAYER
        if (!ok) {
            embed_close(e);
            return false;
        }
        if (!shape2(L->qkv, e->n_embd, (uint64_t)e->n_embd * 3, "attn_qkv",
                    err, errsz) ||
            !shape2(L->attn_out, e->n_embd, e->n_embd, "attn_output", err,
                    errsz) ||
            !shape2(L->ffn_up, e->n_embd, e->n_ff, "ffn_up", err, errsz) ||
            !shape2(L->ffn_gate, e->n_embd, e->n_ff, "ffn_gate", err, errsz) ||
            !shape2(L->ffn_down, e->n_ff, e->n_embd, "ffn_down", err, errsz)) {
            embed_close(e);
            return false;
        }
    }

    /* ---- the configuration §8 records ---- */
    ModelParams *c = &e->cfg;
    c->present = true;
    snprintf(c->model, sizeof c->model, "%s",
             gguf_str(&e->g, "general.name", "unnamed"));
    snprintf(c->arch, sizeof c->arch, "%s", arch);
    c->dim = e->n_embd;
    snprintf(c->pooling, sizeof c->pooling, "%s", e->pool_mean ? "mean" : "cls");
    c->max_tokens = KB_EMBED_MAX_TOKENS;
    if (e->n_ctx && c->max_tokens > e->n_ctx)
        c->max_tokens = e->n_ctx;
    const char *qp, *dp;
    default_prefixes(c->arch, c->model, &qp, &dp);
    snprintf(c->query_prefix, sizeof c->query_prefix, "%s", qp);
    snprintf(c->doc_prefix, sizeof c->doc_prefix, "%s", dp);
    c->normalize = true;
    snprintf(c->quantization, sizeof c->quantization, "int8");
    snprintf(c->weights, sizeof c->weights, "%s",
             file_type_name(gguf_u64(&e->g, "general.file_type", 0)));
    c->tokenizer = KB_WPM_VERSION;

    /* ---- scratch ---- */
    const size_t T = KB_EMBED_MAX_TOKENS;
    const size_t D = e->n_embd;
    e->ids = (int32_t *)arena_alloc(a, T * sizeof(int32_t));
    e->x = (float *)arena_alloc(a, T * D * sizeof(float));
    e->q = (float *)arena_alloc(a, T * D * sizeof(float));
    e->k = (float *)arena_alloc(a, T * D * sizeof(float));
    e->v = (float *)arena_alloc(a, T * D * sizeof(float));
    e->attn = (float *)arena_alloc(a, T * D * sizeof(float));
    e->qkv = (float *)arena_alloc(a, 3 * D * sizeof(float));
    e->tmp = (float *)arena_alloc(a, D * sizeof(float));
    e->lw = (float *)arena_alloc(a, D * sizeof(float));
    e->lb = (float *)arena_alloc(a, D * sizeof(float));
    e->row = (float *)arena_alloc(a, 3 * D * sizeof(float));
    e->ff1 = (float *)arena_alloc(a, e->n_ff * sizeof(float));
    e->ff2 = (float *)arena_alloc(a, e->n_ff * sizeof(float));
    e->scores = (float *)arena_alloc(a, T * sizeof(float));
    return true;
}

void embed_close(Embedder *e) {
    gguf_close(&e->g);
}

/* ---- the forward pass ---------------------------------------------------
 *
 * The order below is this architecture's, and every step of it is wired
 * here rather than read from the file, which is why embed_open refuses any
 * other architecture by name.
 *
 *   embeddings  LayerNorm(token_embd[id] + token_types[0])
 *
 *               There are no learned position embeddings in this model —
 *               the tensor simply is not in the file — because position
 *               enters through the rotation below instead.
 *
 *   per layer   POST-norm, which is BERT's order and not the pre-norm
 *               order most decoder models use:
 *
 *                 a = W_out · Attention(x)
 *                 x = LayerNorm(a + x,  attn_output_norm)
 *                 f = W_down · ( silu(W_gate · x) * (W_up · x) )
 *                 x = LayerNorm(f + x,  layer_output_norm)
 *
 *               The normalisation comes AFTER the residual addition. Moving
 *               it before would be a working network that is not this one.
 *
 *   attention   One fused QKV projection, split into thirds; rotary
 *               position embeddings applied to Q and K and not to V;
 *               softmax over every position, because the model is not
 *               causal and every token may see every other.
 *
 *   pooling     The mean over all tokens, [CLS] and [SEP] included, which
 *               is what this file's pooling_type says. Then L2.
 */

static void layernorm(float *x, const float *w, const float *b, uint32_t n,
                      float eps) {
    float mean = 0.0f;
    for (uint32_t i = 0; i < n; i++)
        mean += x[i];
    mean /= (float)n;
    float var = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = x[i] - mean;
        var += d * d;
    }
    var /= (float)n;
    /* The epsilon is inside the square root, which is where LayerNorm puts
     * it and where this model's 1e-12 was calibrated. */
    const float inv = 1.0f / sqrtf(var + eps);
    for (uint32_t i = 0; i < n; i++)
        x[i] = (x[i] - mean) * inv * w[i] + b[i];
}

/* Rotary position embeddings, applied per head.
 *
 * THIS FILE'S CONVENTION IS THE HALF-SPLIT ONE. Component i pairs with
 * component i + d/2, not with component i + 1. The two conventions are the
 * same rotation applied to a different pairing of the axes, so the wrong one
 * produces a model that still runs, still returns unit vectors, and retrieves
 * badly — which is exactly the failure §8 warns about. The choice here is
 * not a guess: it is what llama.cpp reports for this file's architecture,
 * and it agrees with the model's own published configuration.
 *
 * The angle for pair i is pos · base^(-2i/d), computed by repeated
 * multiplication from pos so that the sequence is the same one ggml walks.
 */
static void rope(float *vec, uint32_t n_head, uint32_t d, int32_t pos,
                 float base) {
    const float theta_scale = powf(base, -2.0f / (float)d);
    for (uint32_t h = 0; h < n_head; h++) {
        float *p = vec + (size_t)h * d;
        float theta = (float)pos;
        for (uint32_t i = 0; i < d / 2; i++) {
            const float c = cosf(theta), s = sinf(theta);
            const float x0 = p[i], x1 = p[i + d / 2];
            p[i] = x0 * c - x1 * s;
            p[i + d / 2] = x0 * s + x1 * c;
            theta *= theta_scale;
        }
    }
}

static bool matvec(const GgufTensor *t, const float *x, float *out) {
    return quant_matvec(t->type, t->data, t->ne[0], t->ne[1], x, out);
}

static bool forward(Embedder *e, const int32_t *ids, size_t T, float *out) {
    const uint32_t D = e->n_embd, H = e->n_head, HD = e->n_head_dim;
    float *type0 = e->row; /* token_types row 0 */
    float *nw = e->row + D;
    float *nb = e->row + 2 * D;
    if (!quant_row(e->type_embd->type, e->type_embd->data, D, type0) ||
        !quant_row(e->embd_norm_w->type, e->embd_norm_w->data, D, nw) ||
        !quant_row(e->embd_norm_b->type, e->embd_norm_b->data, D, nb))
        return false;

    uint64_t emb_row_bytes;
    if (!quant_row_bytes(e->tok_embd->type, D, &emb_row_bytes))
        return false;
    for (size_t t = 0; t < T; t++) {
        float *xt = e->x + t * D;
        if (!quant_row(e->tok_embd->type,
                       e->tok_embd->data + (uint64_t)ids[t] * emb_row_bytes, D,
                       xt))
            return false;
        for (uint32_t i = 0; i < D; i++)
            xt[i] += type0[i];
        layernorm(xt, nw, nb, D, e->eps);
    }

    const float kq_scale = 1.0f / sqrtf((float)HD);
    for (uint32_t l = 0; l < e->n_layer; l++) {
        const EmbedLayer *L = &e->layer[l];
        for (size_t t = 0; t < T; t++) {
            if (!matvec(L->qkv, e->x + t * D, e->qkv))
                return false;
            /* The fused projection is Q, then K, then V, in that order. */
            memcpy(e->q + t * D, e->qkv, D * sizeof(float));
            memcpy(e->k + t * D, e->qkv + D, D * sizeof(float));
            memcpy(e->v + t * D, e->qkv + 2 * D, D * sizeof(float));
            rope(e->q + t * D, H, HD, (int32_t)t, e->rope_base);
            rope(e->k + t * D, H, HD, (int32_t)t, e->rope_base);
        }
        for (size_t t = 0; t < T; t++) {
            float *o = e->attn + t * D;
            memset(o, 0, D * sizeof(float));
            for (uint32_t h = 0; h < H; h++) {
                const float *qh = e->q + t * D + (size_t)h * HD;
                float max = -INFINITY;
                for (size_t u = 0; u < T; u++) {
                    const float *kh = e->k + u * D + (size_t)h * HD;
                    float s = 0.0f;
                    for (uint32_t i = 0; i < HD; i++)
                        s += qh[i] * kh[i];
                    s *= kq_scale;
                    e->scores[u] = s;
                    if (s > max)
                        max = s;
                }
                float sum = 0.0f;
                for (size_t u = 0; u < T; u++) {
                    e->scores[u] = expf(e->scores[u] - max);
                    sum += e->scores[u];
                }
                const float inv = 1.0f / sum;
                float *oh = o + (size_t)h * HD;
                for (size_t u = 0; u < T; u++) {
                    const float p = e->scores[u] * inv;
                    const float *vh = e->v + u * D + (size_t)h * HD;
                    for (uint32_t i = 0; i < HD; i++)
                        oh[i] += p * vh[i];
                }
            }
        }
        if (!quant_row(L->attn_norm_w->type, L->attn_norm_w->data, D, e->lw) ||
            !quant_row(L->attn_norm_b->type, L->attn_norm_b->data, D, e->lb))
            return false;
        for (size_t t = 0; t < T; t++) {
            float *xt = e->x + t * D;
            if (!matvec(L->attn_out, e->attn + t * D, e->tmp))
                return false;
            for (uint32_t i = 0; i < D; i++)
                xt[i] += e->tmp[i];
            layernorm(xt, e->lw, e->lb, D, e->eps);
        }
        if (!quant_row(L->out_norm_w->type, L->out_norm_w->data, D, e->lw) ||
            !quant_row(L->out_norm_b->type, L->out_norm_b->data, D, e->lb))
            return false;
        for (size_t t = 0; t < T; t++) {
            float *xt = e->x + t * D;
            if (!matvec(L->ffn_gate, xt, e->ff1) ||
                !matvec(L->ffn_up, xt, e->ff2))
                return false;
            /* SiLU on the GATE and not on the up-projection: x·sigmoid(x)
             * applied to the wrong half is a different network. */
            for (uint32_t i = 0; i < e->n_ff; i++) {
                const float g = e->ff1[i];
                e->ff1[i] = (g / (1.0f + expf(-g))) * e->ff2[i];
            }
            if (!matvec(L->ffn_down, e->ff1, e->tmp))
                return false;
            for (uint32_t i = 0; i < D; i++)
                xt[i] += e->tmp[i];
            layernorm(xt, e->lw, e->lb, D, e->eps);
        }
    }

    if (e->pool_mean) {
        for (uint32_t i = 0; i < D; i++)
            out[i] = 0.0f;
        for (size_t t = 0; t < T; t++) {
            const float *xt = e->x + t * D;
            for (uint32_t i = 0; i < D; i++)
                out[i] += xt[i];
        }
        const float inv = 1.0f / (float)T;
        for (uint32_t i = 0; i < D; i++)
            out[i] *= inv;
    } else {
        memcpy(out, e->x, D * sizeof(float));
    }
    return true;
}

bool embed_tokens(Embedder *e, const int32_t *ids, size_t n, float *out) {
    if (n == 0 || n > KB_EMBED_MAX_TOKENS)
        return false;
    /* The embedding lookup below is `data + id * row_bytes` straight into the
     * mapping. wpm_encode cannot produce an id outside the vocabulary, but a
     * caller handing ids in directly can, and an unchecked one is a read of
     * whatever follows the weights. */
    for (size_t i = 0; i < n; i++)
        if (ids[i] < 0 || (uint64_t)ids[i] >= e->wpm.n)
            return false;
    if (!forward(e, ids, n, out))
        return false;
    e->n_embedded++;
    e->n_tokens += n;
    return true;
}

bool embed_text(Embedder *e, const char *text, size_t len, bool is_query,
                float *out, bool *truncated) {
    if (truncated)
        *truncated = false;
    /* THE PREFIX. §8's asymmetry lives in this one line, and the two roles
     * must not be able to share a branch by accident. */
    const char *prefix = is_query ? e->cfg.query_prefix : e->cfg.doc_prefix;
    const size_t plen = strlen(prefix);
    if (plen + len + 1 > e->prefixed_cap) {
        size_t nc = (plen + len + 1) * 2;
        e->prefixed = (char *)arena_realloc(e->a, e->prefixed,
                                            e->prefixed_cap, nc);
        e->prefixed_cap = nc;
    }
    memcpy(e->prefixed, prefix, plen);
    memcpy(e->prefixed + plen, text, len);

    size_t cap = e->cfg.max_tokens;
    if (cap > KB_EMBED_MAX_TOKENS)
        cap = KB_EMBED_MAX_TOKENS;
    size_t T = wpm_encode(e->a, &e->wpm, e->prefixed, plen + len, true, e->ids,
                          cap, truncated);
    if (T == 0)
        return false;
    if (!embed_tokens(e, e->ids, T, out))
        return false;

    if (e->cfg.normalize) {
        double s = 0.0;
        for (uint32_t i = 0; i < e->n_embd; i++)
            s += (double)out[i] * (double)out[i];
        /* A zero vector has no direction to normalise to; leaving it as it
         * is keeps the scan below well-defined instead of producing NaNs
         * that would poison every comparison it took part in. */
        if (s > 0.0) {
            const float inv = (float)(1.0 / sqrt(s));
            for (uint32_t i = 0; i < e->n_embd; i++)
                out[i] *= inv;
        }
    }
    return true;
}

/* ---- identity ----------------------------------------------------------- */

void model_fingerprint(const ModelParams *m, char out[65]) {
    /* Field-separated with a byte that cannot occur in any of them, so two
     * different configurations cannot concatenate to the same string. */
    char buf[1024];
    int32_t n = snprintf(buf, sizeof buf,
                         "1\x1f%s\x1f%s\x1f%lu\x1f%s\x1f%lu\x1f%s\x1f%s\x1f%d"
                         "\x1f%s\x1f%s\x1f%lu",
                         m->model, m->arch, (unsigned long)m->dim, m->pooling,
                         (unsigned long)m->max_tokens, m->query_prefix,
                         m->doc_prefix, m->normalize ? 1 : 0, m->quantization,
                         m->weights, (unsigned long)m->tokenizer);
    if (n < 0)
        n = 0;
    if (n > (int32_t)sizeof buf)
        n = (int32_t)sizeof buf;
    sha256_hex(buf, (size_t)n, out);
}

bool model_params_equal(const ModelParams *a, const ModelParams *b) {
    char fa[65], fb[65];
    if (a->present != b->present)
        return false;
    model_fingerprint(a, fa);
    model_fingerprint(b, fb);
    return strcmp(fa, fb) == 0;
}

void model_params_diff(const ModelParams *s, const ModelParams *l, char *out,
                       size_t outsz) {
#define DIFF_S(field, label)                                                   \
    if (strcmp(s->field, l->field) != 0) {                                     \
        snprintf(out, outsz, label " is \"%s\" in the index and \"%s\" in the "\
                                   "loaded model",                             \
                 s->field, l->field);                                          \
        return;                                                                \
    }
    DIFF_S(model, "model")
    DIFF_S(arch, "architecture")
    DIFF_S(pooling, "pooling")
    DIFF_S(weights, "weight quantisation")
    DIFF_S(quantization, "vector quantisation")
    DIFF_S(query_prefix, "the query prefix")
    DIFF_S(doc_prefix, "the document prefix")
#undef DIFF_S
    if (s->dim != l->dim) {
        snprintf(out, outsz, "dim is %lu in the index and %lu in the loaded "
                             "model",
                 (unsigned long)s->dim, (unsigned long)l->dim);
        return;
    }
    if (s->max_tokens != l->max_tokens) {
        snprintf(out, outsz,
                 "maxTokens is %lu in the index and %lu in the loaded model",
                 (unsigned long)s->max_tokens, (unsigned long)l->max_tokens);
        return;
    }
    if (s->normalize != l->normalize) {
        snprintf(out, outsz, "normalize is %s in the index and %s in the "
                             "loaded model",
                 s->normalize ? "true" : "false",
                 l->normalize ? "true" : "false");
        return;
    }
    if (s->tokenizer != l->tokenizer) {
        snprintf(out, outsz,
                 "the tokenizer is version %lu in the index and %lu in this "
                 "build",
                 (unsigned long)s->tokenizer, (unsigned long)l->tokenizer);
        return;
    }
    snprintf(out, outsz, "the configurations differ");
}
