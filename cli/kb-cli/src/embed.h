/* The embedding model (§8): load it, run it, get a vector.
 *
 * WHAT THIS LOADS. Whatever the GGUF says it is. The architecture, the layer
 * count, the embedding width, the head count, the normalisation epsilon, the
 * rotary base, the pooling and the entire tokenizer come out of the file's
 * own metadata; nothing here assumes a model. What IS fixed is the shape of
 * network this build knows how to run — a non-causal post-norm encoder with
 * fused QKV, rotary position embeddings and a gated feed-forward — and a
 * file describing anything else is refused by name rather than run as if it
 * were one of these.
 *
 * THE PREFIXES ARE PART OF THE CONFIGURATION, AND THAT IS §8's POINT. These
 * models are asymmetric: the same sentence is meant to be embedded one way
 * as a question and another way as an answer, and the only difference is a
 * marker glued to the front. Getting it wrong "degrades retrieval while
 * breaking nothing visibly" — the vectors still have unit norm, the search
 * still returns ten hits, and they are quietly worse. So the prefixes are
 * chosen from the model's identity, written into index/model.json, and
 * compared on every search.
 *
 * WHAT COMES OUT. One vector of `dim` floats, mean- or CLS-pooled as the
 * file says, and L2-normalised when the configuration says to normalise.
 * Normalisation is what makes a dot product a cosine, which is what makes
 * the int8 scan in vectors.c a similarity search.
 */
#ifndef KB_EMBED_H
#define KB_EMBED_H

#include "gguf.h"
#include "wpm.h"

/* The longest token sequence a single text is embedded from. Chunks target
 * KB_CHUNK_TOKENS (400) estimated tokens and this is comfortably above what
 * that many bytes of even very dense source code produces, so truncation is
 * a rare event rather than a routine one — and when it happens the caller is
 * told, because a chunk the model only saw the front of is a chunk whose
 * vector does not describe all of it. */
#define KB_EMBED_MAX_TOKENS 1024u

/* Exactly what index/model.json records (§8), and exactly what a mismatch
 * is computed over. Fixed-size strings so a ModelParams can be compared,
 * copied and stack-allocated without an arena. */
typedef struct {
    bool present; /* model.json records a model at all */
    char model[128];
    char arch[64];
    uint32_t dim;
    char pooling[16];
    uint32_t max_tokens;
    char query_prefix[128];
    char doc_prefix[128];
    bool normalize;
    char quantization[16]; /* how VECTORS are stored: "int8" */
    char weights[16];      /* how the MODEL is stored: "Q4_K" */
    uint32_t tokenizer;    /* KB_WPM_VERSION */
} ModelParams;

typedef struct {
    const GgufTensor *qkv, *attn_out;
    const GgufTensor *attn_norm_w, *attn_norm_b;
    const GgufTensor *ffn_up, *ffn_gate, *ffn_down;
    const GgufTensor *out_norm_w, *out_norm_b;
} EmbedLayer;

typedef struct {
    Arena *a;
    Gguf g;
    Wpm wpm;
    ModelParams cfg;
    char path[KB_PATH_MAX];

    uint32_t n_embd, n_layer, n_head, n_ff, n_ctx, n_head_dim;
    float eps, rope_base;
    bool pool_mean;

    const GgufTensor *tok_embd, *type_embd, *embd_norm_w, *embd_norm_b;
    EmbedLayer *layer;

    /* Scratch, sized once for KB_EMBED_MAX_TOKENS so a per-call allocation
     * never happens inside the loop that embeds a whole corpus. */
    int32_t *ids;
    float *x, *q, *k, *v, *attn;   /* T × n_embd each */
    float *qkv;                    /* 3 × n_embd */
    float *tmp, *lw, *lb, *row;    /* n_embd each (row is 3 × n_embd) */
    float *ff1, *ff2;              /* n_ff each */
    float *scores;                 /* T */
    char *prefixed;
    size_t prefixed_cap;

    uint64_t n_embedded; /* texts embedded, for timings */
    uint64_t n_tokens;
} Embedder;

/* Where the weights are: $KB_MODEL if set, else the single *.gguf under
 * ~/.kb/models (or $KB_STORE/../models when KB_STORE is set). Returns false
 * with a message naming what it looked for — §11's model_missing wants "the
 * expected weights path" and this is it. */
bool embed_find_model(Arena *a, char *out, size_t outsz, char *err,
                      size_t errsz);

bool embed_open(Arena *a, const char *path, Embedder *e, char *err,
                size_t errsz);
void embed_close(Embedder *e);

/* One text to one vector of e->n_embd floats. `is_query` picks which of the
 * two prefixes is glued on, and that is the ONLY difference between the two
 * calls — which is why a test that gives them the same text and expects
 * different vectors detects a prefix that is not being applied. */
bool embed_text(Embedder *e, const char *text, size_t len, bool is_query,
                float *out, bool *truncated);

/* A stable fingerprint of a configuration: the sha256 of every field that
 * changes what a vector means, in a fixed order. Two stores whose vectors
 * are comparable have the same fingerprint, and §8's mismatch is exactly
 * this string differing. */
void model_fingerprint(const ModelParams *m, char out[65]);
/* True when the loaded configuration is the one the store recorded. */
bool model_params_equal(const ModelParams *a, const ModelParams *b);
/* Why they differ, as one sentence naming the first field that does. */
void model_params_diff(const ModelParams *stored, const ModelParams *loaded,
                       char *out, size_t outsz);

#endif /* KB_EMBED_H */
