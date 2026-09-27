/* A folder's history: the chunk files of .lap/log/ that make it, in order,
 * read as one stream of bytes.
 *
 * A chunk is named <lineage>.<n>.jsonl, n counting that lineage's chunks
 * from 1 (six digits, zero-padded). A lineage's open chunk is its highest
 * n; every lower one is sealed and never written again. Order is the hash
 * chain: a chunk's first record carries the previous chunk's last hash as
 * prev, so no manifest lists the chunks.
 *
 * Offsets into a history are positions in the concatenation of its chunks.
 * Only the last chunk ever grows, so an offset, once handed out, names the
 * same byte for as long as the sealed chunks are untouched: the index keeps
 * offsets exactly as it did for one file.
 */
#ifndef LAP_HIST_H
#define LAP_HIST_H

#include "platform.h"

#define HIST_LINEAGE_MAX 16

typedef struct {
    char lineage[HIST_LINEAGE_MAX];
    int32_t n;
    char name[64];  /* "main.000003.jsonl" */
    uint64_t start; /* offset of its first byte in the history */
    uint64_t size;
} HistChunk;

typedef struct {
    char dir[LAP_PATH_MAX];              /* <root>/.lap/log */
    char lineage[HIST_LINEAGE_MAX];      /* the lineage this folder writes */
    HistChunk *v;
    int32_t n;
    int32_t cap;
    uint64_t size; /* bytes in the whole history */
    uint64_t limit; /* the chunk limit appends seal at; hist_chunk_limit() */
} Hist;

/* "main.000003.jsonl" -> lineage "main", n 3. False for any other name:
 * temp files, stray files, a lineage that is not "main" or 12 hex digits. */
bool hist_parse_name(const char *name, char lineage[HIST_LINEAGE_MAX],
                     int32_t *n);
/* The name of chunk n of a lineage. */
void hist_chunk_name(const char *lineage, int32_t n, char out[64]);

/* The chunk limit: LAP_CHUNK_BYTES, or LAP_TEST_CHUNK_BYTES when set. */
uint64_t hist_chunk_limit(void);

/* Lists lineage's chunks in lapdir/log. A missing directory or no chunk is
 * an empty history, not an error: the first append creates chunk 1. Chunk
 * numbers must run 1, 2, ... without a gap, else false with a reason. */
bool hist_open(Arena *a, const char *lapdir, const char *lineage, Hist *h,
               char *err, size_t errsz);

/* The path of chunk i of h. */
void hist_chunk_path(const Hist *h, int32_t i, char *out, size_t outsz);
/* The chunk holding offset off (the last chunk for off == size), or -1. */
int32_t hist_locate(const Hist *h, uint64_t off);
/* True for every chunk but the open one (the last). */
bool hist_is_sealed(const Hist *h, int32_t i);
/* "main.000002.jsonl line 7": where an offset is, for people. */
void hist_where(const Hist *h, const char *data, uint64_t off, char *out,
                size_t outsz);

/* Reads [off, off+len), across chunk boundaries, NUL-terminated. */
bool hist_read(Arena *a, const Hist *h, uint64_t off, size_t len, char **out);
bool hist_read_all(Arena *a, const Hist *h, char **data, size_t *len);

/* The hash of the history's last complete record, or 64 zeros when it has
 * none. An unterminated line at the end of the open chunk is skipped. */
bool hist_tail_hash(Arena *a, const Hist *h, char out[65]);

/* Writers (lock held) only. */

/* Truncates an unterminated final line off the open chunk, reporting it on
 * stderr. */
bool hist_repair_torn_tail(Arena *a, Hist *h);
/* Appends one record's line (with its '\n'). When the open chunk holds
 * something and the line would take it past the limit, the line starts the
 * next chunk instead. */
bool hist_append(Arena *a, Hist *h, const char *line, size_t len, char *err,
                 size_t errsz);
/* Seals the open chunk by creating the next one, empty. An empty open chunk
 * is left as it is. */
bool hist_seal(Arena *a, Hist *h, char *err, size_t errsz);

#endif /* LAP_HIST_H */
