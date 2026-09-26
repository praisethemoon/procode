/* Times the embedder on real text, and checks that a faster build still
 * computes the same vectors.
 *
 *   kb-embed-bench <model.gguf> [--chunks N] [--piece B] [--q8] [--save F | --check F] <file>...
 *
 * The files are cut into B-byte pieces (default 1600, the size kb's chunker
 * aims for; smaller pieces show how the embedder copes with short chunks),
 * and the first N pieces (default 64) are embedded one after another, as
 * `kb add` does. It prints milliseconds per chunk and tokens per second.
 * --save writes the vectors to F; --check compares against a saved F and
 * prints the smallest cosine and the largest absolute difference, so an
 * optimisation that changes the arithmetic shows by how much. --q8 runs the
 * layers in int8 (embed_quantize) and reports how long the conversion took.
 */

#include "../../src/embed.h"
#include "../../src/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *model = NULL, *save = NULL, *check = NULL;
    size_t want = 64, piece = 1600;
    bool q8 = false;
    const char **files = (const char **)calloc((size_t)argc, sizeof(char *));
    size_t nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--chunks") == 0 && i + 1 < argc)
            want = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--q8") == 0)
            q8 = true;
        else if (strcmp(argv[i], "--piece") == 0 && i + 1 < argc)
            piece = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc)
            save = argv[++i];
        else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc)
            check = argv[++i];
        else if (!model)
            model = argv[i];
        else
            files[nfiles++] = argv[i];
    }
    if (!model || nfiles == 0) {
        fprintf(stderr, "usage: kb-embed-bench <model.gguf> [--chunks N] [--piece B] "
                        "[--save F | --check F] <file>...\n");
        return 2;
    }
    Arena *a = arena_new(1 << 26);
    char err[512];
    Embedder e;
    if (!embed_open(a, model, &e, err, sizeof err)) {
        fprintf(stderr, "open: %s\n", err);
        return 1;
    }
    if (q8) {
        double c0 = now();
        if (!embed_quantize(&e)) {
            fprintf(stderr, "int8 conversion failed\n");
            return 1;
        }
        printf("int8 conversion: %.0f ms\n", (now() - c0) * 1e3);
    }
    const char **text = (const char **)arena_alloc(a, want * sizeof(char *));
    size_t *len = (size_t *)arena_alloc(a, want * sizeof(size_t));
    size_t n = 0;
    for (size_t f = 0; f < nfiles && n < want; f++) {
        char *data;
        size_t dlen;
        if (!plat_read_file(a, files[f], &data, &dlen))
            continue;
        for (size_t off = 0; off < dlen && n < want; off += piece) {
            text[n] = data + off;
            len[n] = dlen - off < piece ? dlen - off : piece;
            n++;
        }
    }
    float *vecs = (float *)arena_alloc(a, n * e.n_embd * sizeof(float));
    uint64_t tokens0 = e.n_tokens;
    double t0 = now();
    for (size_t i = 0; i < n; i++) {
        bool truncated;
        if (!embed_text(&e, text[i], len[i], false, vecs + i * e.n_embd, &truncated)) {
            fprintf(stderr, "chunk %zu failed\n", i);
            return 1;
        }
    }
    double dt = now() - t0;
    uint64_t tokens = e.n_tokens - tokens0;
    printf("%zu chunks, %llu tokens: %.1f ms/chunk, %.0f tokens/s\n", n,
           (unsigned long long)tokens, dt * 1e3 / (double)n, (double)tokens / dt);

    if (save) {
        FILE *f = fopen(save, "wb");
        if (!f || fwrite(vecs, sizeof(float), n * e.n_embd, f) != n * e.n_embd) {
            fprintf(stderr, "cannot write %s\n", save);
            return 1;
        }
        fclose(f);
    }
    if (check) {
        float *ref = (float *)arena_alloc(a, n * e.n_embd * sizeof(float));
        FILE *f = fopen(check, "rb");
        if (!f || fread(ref, sizeof(float), n * e.n_embd, f) != n * e.n_embd) {
            fprintf(stderr, "cannot read %s (same files and --chunks?)\n", check);
            return 1;
        }
        fclose(f);
        double worst = 1.0, maxdiff = 0.0;
        for (size_t i = 0; i < n; i++) {
            const float *x = vecs + i * e.n_embd, *y = ref + i * e.n_embd;
            double dot = 0, nx = 0, ny = 0;
            for (uint32_t k = 0; k < e.n_embd; k++) {
                dot += (double)x[k] * y[k];
                nx += (double)x[k] * x[k];
                ny += (double)y[k] * y[k];
                double d = fabs((double)x[k] - y[k]);
                if (d > maxdiff)
                    maxdiff = d;
            }
            double c = dot / (sqrt(nx) * sqrt(ny));
            if (c < worst)
                worst = c;
        }
        printf("against %s: worst cosine %.7f, largest difference %.2e\n", check,
               worst, maxdiff);
    }
    embed_close(&e);
    return 0;
}
