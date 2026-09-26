#include "modelrec.h"

#include "json.h"
#include "platform.h"
#include "sha256.h"

#include <string.h>

void model_probe(Arena *a, ModelProbe *out) {
    memset(out, 0, sizeof *out);
    if (!embed_models_dir(out->dir, sizeof out->dir))
        snprintf(out->dir, sizeof out->dir, "~/.kb/models");
    if (!embed_find_model(a, out->path, sizeof out->path, out->err,
                          sizeof out->err))
        return;
    Embedder e;
    char why[512];
    if (!embed_open(a, out->path, &e, why, sizeof why)) {
        snprintf(out->err, sizeof out->err, "%s cannot be loaded: %s",
                 out->path, why);
        return;
    }
    out->params = e.cfg;
    out->bytes = (uint64_t)e.g.len;
    embed_close(&e);
    out->found = true;
}

static void copy_str(char *dst, size_t dstsz, const JVal *v, const char *key) {
    const char *s = jobj_str(v, key);
    snprintf(dst, dstsz, "%s", s ? s : "");
}

bool model_recorded(Arena *a, const Store *s, ModelParams *out,
                    char sha256[65]) {
    memset(out, 0, sizeof *out);
    sha256[0] = '\0';
    char path[KB_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s->dir, KB_MODEL_NAME);
    char *data;
    size_t len;
    if (!plat_read_file_max(a, path, &data, &len, (size_t)1 << 20))
        return false;
    char jerr[128];
    JVal *v = json_parse(a, data, len, jerr, sizeof jerr);
    if (!v || v->t != J_OBJ || !jobj_str(v, "model"))
        return false;
    out->present = true;
    copy_str(out->model, sizeof out->model, v, "model");
    copy_str(out->arch, sizeof out->arch, v, "arch");
    out->dim = (uint32_t)jobj_int(v, "dim", 0);
    copy_str(out->pooling, sizeof out->pooling, v, "pooling");
    out->max_tokens = (uint32_t)jobj_int(v, "maxTokens", 0);
    copy_str(out->query_prefix, sizeof out->query_prefix, v, "queryPrefix");
    copy_str(out->doc_prefix, sizeof out->doc_prefix, v, "documentPrefix");
    out->normalize = jobj_bool(v, "normalize", false);
    copy_str(out->quantization, sizeof out->quantization, v, "quantization");
    copy_str(out->weights, sizeof out->weights, v, "weights");
    out->tokenizer = (uint32_t)jobj_int(v, "tokenizer", 0);
    const JVal *file = jobj_get(v, "modelFile");
    const char *h = file ? jobj_str(file, "sha256") : NULL;
    if (h && strlen(h) == 64)
        memcpy(sha256, h, 65);
    return true;
}

void model_params_json(StrBuf *sb, const ModelParams *m) {
    sb_puts(sb, "\"model\":");
    json_escape_c(sb, m->model);
    sb_puts(sb, ",\"arch\":");
    json_escape_c(sb, m->arch);
    sb_printf(sb, ",\"dim\":%lu,\"pooling\":", (unsigned long)m->dim);
    json_escape_c(sb, m->pooling);
    sb_printf(sb, ",\"maxTokens\":%lu,\"queryPrefix\":",
              (unsigned long)m->max_tokens);
    json_escape_c(sb, m->query_prefix);
    sb_puts(sb, ",\"documentPrefix\":");
    json_escape_c(sb, m->doc_prefix);
    sb_printf(sb, ",\"normalize\":%s,\"quantization\":",
              m->normalize ? "true" : "false");
    json_escape_c(sb, m->quantization);
    sb_puts(sb, ",\"weights\":");
    json_escape_c(sb, m->weights);
    char fp[65];
    model_fingerprint(m, fp);
    sb_printf(sb, ",\"tokenizer\":%lu,\"fingerprint\":\"%s\"",
              (unsigned long)m->tokenizer, fp);
}

/* The sha256 of the model file, read through a mapping so 80 MB of weights
 * are not copied to be hashed. */
static bool file_sha256(Arena *a, const char *path, char out[65]) {
    const uint8_t *base;
    size_t len;
    PlatMap *map = plat_map_file(a, path, &base, &len);
    if (!map)
        return false;
    sha256_hex(base, len, out);
    plat_unmap_file(map);
    return true;
}

bool model_record(Arena *a, Store *s, const ModelProbe *m, char *err,
                  size_t errsz) {
    if (!s->lock) {
        snprintf(err, errsz, "internal: model.json written without the lock");
        return false;
    }
    char sha[65];
    if (!file_sha256(a, m->path, sha)) {
        snprintf(err, errsz, "cannot read %s to hash it", m->path);
        return false;
    }
    ChunkParams cp = store_chunk_params(a, s);
    StrBuf sb;
    sb_init(&sb, a);
    sb_puts(&sb, "{\"chunker\":");
    json_escape_c(&sb, cp.chunker);
    sb_printf(&sb, ",\"chunkTokens\":%lu,\"chunkOverlap\":%lu,",
              (unsigned long)cp.chunk_tokens, (unsigned long)cp.chunk_overlap);
    model_params_json(&sb, &m->params);
    const char *name = strrchr(m->path, '/');
    sb_puts(&sb, ",\"modelFile\":{\"name\":");
    json_escape_c(&sb, name ? name + 1 : m->path);
    sb_printf(&sb, ",\"bytes\":%llu,\"sha256\":\"%s\"}}\n",
              (unsigned long long)m->bytes, sha);
    char path[KB_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s->dir, KB_MODEL_NAME);
    if (!plat_mkdirs(s->index_dir) ||
        !plat_write_file_atomic(path, sb.data, sb.len)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    return true;
}

bool model_record_if_absent(Arena *a, Store *s, char *err, size_t errsz) {
    ModelParams recorded;
    char sha[65];
    if (model_recorded(a, s, &recorded, sha))
        return true;
    ModelProbe m;
    model_probe(a, &m);
    if (!m.found)
        return true;
    return model_record(a, s, &m, err, errsz);
}
