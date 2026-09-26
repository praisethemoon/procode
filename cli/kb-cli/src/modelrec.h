/* The model half of index/model.json (§8): which model the store's vectors
 * were produced by, and which model this machine would load now.
 *
 * THE FILE IS FLAT, AS §8 WRITES IT. The chunking parameters store.c owns and
 * the model's own fields sit side by side — `model`, `dim`, `pooling`,
 * `maxTokens`, `queryPrefix`, `documentPrefix`, `normalize`, `quantization` —
 * plus what §8's example leaves out and a mismatch needs: the architecture,
 * how the weights were quantised, the tokenizer's version, and the model
 * file itself (name, size, sha256). The fingerprint over the fields that
 * change what a vector means is recorded too, so a reader can compare two
 * stores without recomputing it.
 *
 * RECORDED AT FIRST INGEST WHEN A MODEL IS THERE, AND BY `kb reindex`. Never
 * silently rewritten: a different file in ~/.kb/models is a mismatch to be
 * reported, not a configuration to adopt.
 */
#ifndef KB_MODELREC_H
#define KB_MODELREC_H

#include "embed.h"
#include "store.h"

/* What ~/.kb/models holds now. `found` false leaves `err` holding
 * model_missing's message (the directory and the curl command). */
typedef struct {
    bool found;
    char dir[KB_PATH_MAX];
    char path[KB_PATH_MAX];
    uint64_t bytes;
    ModelParams params;
    char err[1024];
} ModelProbe;

/* Finds and opens the model, reads its configuration, and closes it. */
void model_probe(Arena *a, ModelProbe *out);

/* The model model.json records, or false when it records none. `sha256` is
 * the recorded model file's hash ("" when absent). */
bool model_recorded(Arena *a, const Store *s, ModelParams *out,
                    char sha256[65]);

/* Writes the probed model into model.json beside the chunking parameters the
 * file already records (or this build's, when there is no file yet). Hashes
 * the model file. Needs the write lock. */
bool model_record(Arena *a, Store *s, const ModelProbe *m, char *err,
                  size_t errsz);

/* Records the model only when model.json names none and one is found. The
 * first-ingest rule; a missing model is not an error here, the store is then
 * keyword-only. */
bool model_record_if_absent(Arena *a, Store *s, char *err, size_t errsz);

/* The configuration as JSON object members (no braces), for status and for
 * model_mismatch's details. */
void model_params_json(StrBuf *sb, const ModelParams *m);

#endif
