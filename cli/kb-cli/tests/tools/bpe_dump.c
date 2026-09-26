/* Tokenises each file named on the command line with kb's byte-level BPE and
 * prints one JSON line per file: {"file": ..., "ids": [...]}, with [CLS] and
 * [SEP]. A development tool: tools/modernbert/check_tokenizer.py compares its
 * output with the Hugging Face tokenizer.
 *
 *   bin/kb-bpe-dump <model.gguf> <file>...
 */
#include <stdio.h>
#include <stdlib.h>

#include "arena.h"
#include "bpe.h"
#include "gguf.h"
#include "platform.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: kb-bpe-dump <model.gguf> <file>...\n");
        return 2;
    }
    Arena *a = arena_new(1 << 24);
    char err[512];
    Gguf g;
    Bpe b;
    if (!gguf_open(a, argv[1], &g, err, sizeof err) || !bpe_init(a, &g, &b, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        Arena *t = arena_new(1 << 24);
        char *data;
        size_t len;
        if (!plat_read_file(t, argv[i], &data, &len)) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            arena_free(t);
            continue;
        }
        size_t cap = len * 2 + 16;
        int32_t *ids = (int32_t *)arena_alloc(t, cap * sizeof(int32_t));
        bool truncated;
        size_t n = bpe_encode(t, &b, data, len, true, ids, cap, &truncated);
        printf("{\"file\":\"%s\",\"ids\":[", argv[i]);
        for (size_t k = 0; k < n; k++)
            printf(k ? ",%d" : "%d", ids[k]);
        printf("]}\n");
        arena_free(t);
    }
    arena_free(a);
    return 0;
}
