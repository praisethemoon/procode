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
#include "rec.h"

#define HIST_LINEAGE_MAX 16
/* the highest chunk number: names hold six digits */
#define HIST_MAX_CHUNK 999999

typedef struct {
    char lineage[HIST_LINEAGE_MAX];
    int32_t n;
    char name[64];  /* "main.000003.jsonl" */
    uint64_t start; /* offset of its first byte in the history */
    uint64_t size;
    const char *label; /* in a view, its branch's name; NULL for main */
} HistChunk;

typedef struct {
    char dir[LAP_PATH_MAX];              /* <root>/.lap/log */
    char lineage[HIST_LINEAGE_MAX];      /* the lineage this folder writes */
    HistChunk *v;
    int32_t n;
    int32_t cap;
    uint64_t size; /* bytes in the whole history */
    uint64_t limit; /* the chunk limit appends seal at; hist_chunk_limit() */
    bool legacy; /* read from the single-file log.jsonl of before chunks */
    /* that file's bytes, read once when opened: a conversion may remove it
     * before this reader is done (readers take no lock) */
    const char *mem;
    /* A branch folder's history: its parent's chunks 1..base_chunk, then
     * its own. parent is "" in a main folder. */
    char parent[HIST_LINEAGE_MAX];
    char base[65];
    int32_t base_chunk;
    char name[128]; /* the branch's name, from its branch record */
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
 * numbers must run 1, 2, ... without a gap, else false with a reason.
 * With no main chunk, a log.jsonl of before chunks is read as the one chunk
 * of a legacy history; readers never convert it. */
bool hist_open(Arena *a, const char *lapdir, const char *lineage, Hist *h,
               char *err, size_t errsz);

/* The lineage a folder writes: the id in lapdir/lineage, or "main" when
 * there is no such file. False, with a reason, for a file that does not
 * hold a branch id. */
bool hist_folder_lineage(Arena *a, const char *lapdir,
                         char out[HIST_LINEAGE_MAX], char *err, size_t errsz);
/* Writes lapdir/lineage. */
bool hist_write_lineage(const char *lapdir, const char *lineage);
/* Writes chunk `name` into logdir whole, atomically, its temp file in the
 * folder above (.lap/) so none is ever in log/. */
bool hist_write_chunk(const char *logdir, const char *name, const void *data,
                      size_t len);
/* Removes the temp files in lapdir/log: under the lock every one is a
 * leftover (of a crash, or of a lap that wrote them there). */
void hist_clear_tmp(Arena *a, const char *lapdir);

/* A folder's whole history: for main, hist_open's; for a branch, the
 * parent's chunks up to the base chunk its branch record names, then the
 * branch's own chunks. */
bool hist_open_folder(Arena *a, const char *lapdir, Hist *h, char *err,
                      size_t errsz);
/* The history of branch `lineage` as this folder holds it: its parent's
 * history up to its base chunk, then its own. How a parent reads a branch
 * whose chunks it has (after a git merge, or copied by lap merge). A
 * branch of a branch is read the same way, one branch record at a time up
 * to main: main's chunks to the first branch's base, that branch's to the
 * next one's base, and so on. */
bool hist_open_lineage(Arena *a, const char *lapdir, const char *lineage,
                       Hist *h, char *err, size_t errsz);
/* hist_open_lineage (hist_open for main), cut after the lineage's own
 * chunk `upto`; all of it for upto < 1. A branch's parent part is its
 * parent's view to the base chunk. */
bool hist_open_view(Arena *a, const char *lapdir, const char *lineage,
                    int32_t upto, Hist *h, char *err, size_t errsz);
/* The branch lineages whose first chunk is in lapdir/log, as ids. */
int32_t hist_lineages(Arena *a, const char *lapdir, const char ***out);

/* The first record of a lineage's chunk 1, which for a branch is its
 * branch record. False when there is none or it does not parse. */
bool hist_first_record(Arena *a, const char *lapdir, const char *lineage,
                       Rec *out, char *err, size_t errsz);

/* What one pass over a history finds, a chunk at a time and each record in
 * a scratch arena, so it costs a chunk's memory, not the history's. */
typedef struct {
    int32_t records;
    bool chain_ok;
    uint64_t torn_bytes;      /* an unterminated final line, dropped */
    int32_t unknown_n;        /* records of a type a newer lap wrote */
    const char *unknown_type; /* the first one's, in the caller's arena */
    char last_hash[65];
} HistScan;
/* False only when a chunk cannot be read or a record parsed (err says). */
bool hist_scan(Arena *a, const Hist *h, HistScan *out, char *err,
               size_t errsz);

/* Names, in log->chain_err, the chunk to blame for the chain break that
 * rec_log_parse found in h's bytes (data, as hist_read_all read them). */
void hist_name_break(const Hist *h, const char *data, RecLog *log);

/* The damage a history can show without being read whole: a sealed chunk
 * (any but the last) not ending in a newline — cut or copied short — and,
 * with chain, a chunk whose first record does not chain from the last
 * record of the chunk before it (a stray chunk, one truncated at a line,
 * a wrong prev). False with a message naming the chunk. A few small reads
 * per chunk: cheap enough for every write. */
bool hist_check(Arena *a, const Hist *h, bool chain, char *err, size_t errsz);

/* The path of chunk i of h. */
void hist_chunk_path(const Hist *h, int32_t i, char *out, size_t outsz);
/* The chunk holding offset off (the last chunk for off == size), or -1. */
int32_t hist_locate(const Hist *h, uint64_t off);
/* The branch chunk i belongs to, as people name it: "main", a branch's name
 * for every branch a view spans (this one and those it started from), else
 * the lineage id. */
const char *hist_label(const Hist *h, int32_t i);
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

/* Splits lapdir/log.jsonl into main chunks at the limit, record by record,
 * then removes it; records and hashes are unchanged. A torn final line is
 * dropped, as a writer would. Chunks are built in .lap/log.converting and
 * published as log/ with one rename, the old file removed last, so a crash
 * leaves either the old file or complete chunks: when a writer finds both,
 * chunks that are a prefix of the old file are
 * written again, an old file that is a prefix of the chunks is a leftover
 * and removed, and anything else is refused. *converted reports a split. */
bool hist_convert_legacy(Arena *a, const char *lapdir, uint64_t limit,
                         bool *converted, char *err, size_t errsz);

#endif /* LAP_HIST_H */
