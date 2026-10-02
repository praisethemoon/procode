/* The acceleration index: one fixed-width entry per log record, plus a path
 * table and per-file head slots. All disposable cache — any writer (or
 * `lap rebuild`) reconstructs it from the log; readers finding it absent or
 * stale fall back to a full log scan.
 *
 * Files (native-endian; a single-machine cache, never transported):
 *   .lap/index   IdxHeader, then IdxEntry[count]
 *   .lap/paths   one repo-relative path per line; file_id = line number
 *   .lap/heads   FileHead[npaths]: chain tail + snapshot byte budget
 */
#ifndef LAP_IDX_H
#define LAP_IDX_H

#include "repo.h"

enum {
    IDX_INIT,
    IDX_COMMIT,
    IDX_SESSION_START,
    IDX_SESSION_END,
    IDX_BRANCH,
    IDX_MERGE,
    IDX_UNKNOWN, /* a record type a newer lap wrote */
    IDX_AMEND
};
enum { IDX_OP_EDIT, IDX_OP_CREATE, IDX_OP_DELETE, IDX_OP_UNTRACK };

/* rec_op_gone for an index entry's op. */
static inline bool idx_op_gone(uint8_t op) {
    return op == IDX_OP_DELETE || op == IDX_OP_UNTRACK;
}

/* A commit's op as the index stores it. */
uint8_t idx_op_of(const char *op);

typedef struct {
    char magic[8];    /* "LAPIDX04" */
    uint64_t covered; /* history bytes these entries describe */
    uint64_t count;
    uint32_t commits;      /* total commits indexed */
    uint32_t sessions;     /* highest session number seen */
    uint32_t open_session; /* active session number; 0 = none */
    uint32_t unknown; /* records of a type a newer lap wrote (0 in indexes
                         from before this field: it was padding) */
    /* the last record covered: where it is and the SHA-256 of its bytes,
     * so a history rewritten to the same size is not taken as covered */
    uint64_t tail_off;
    uint32_t tail_len;
    uint32_t pad;
    uint8_t tail[32];
} IdxHeader;

typedef struct {
    uint64_t off;     /* record's byte offset in the history (hist.h) */
    uint32_t len;     /* record length, excluding '\n' */
    uint32_t id;      /* commits so far (monotonic; binary-searchable) */
    uint8_t kind;
    uint8_t op;       /* commits only */
    uint8_t pad[2];
    uint32_t file_id; /* into paths; UINT32_MAX for non-commits */
    uint32_t session; /* numeric S id; 0 = none */
    int64_t prev_same_file; /* entry index of file's previous commit; -1 */
    uint64_t ts;            /* epoch seconds */
    uint32_t old_start, old_lines, new_start, new_lines;
} IdxEntry;

typedef struct {
    int64_t head;         /* entry index of file's last commit; -1 */
    int64_t snap_at;      /* entry index of last snapshot base; -1 */
    uint64_t delta_bytes; /* record bytes accumulated since last snapshot */
    uint64_t delta_count;
} FileHead;

typedef struct Idx {
    IdxHeader h;
    IdxEntry *v;
    char **paths;
    FileHead *heads;
    int32_t npaths;
    /* the amend records, decoded on the first idx_fetch of a commit
     * (amends_read false until then), in the arena the index was loaded
     * in, which outlives any the fetch was given */
    Arena *arena;
    Rec *amends;
    int64_t *amend_at; /* each one's entry */
    int32_t amends_n;
    bool amends_read;
} Idx;

/* The index's header, read only (it may cover less than the history now):
 * false when there is no usable index. */
bool idx_header(Arena *a, const Repo *r, IdxHeader *out);
/* idx_header, only when what it describes is still the history: it covers
 * no more than there is, and the last record it covers is still the one it
 * hashed (a record rewritten in place at the same size is not). */
bool idx_header_trusted(Arena *a, const Repo *r, IdxHeader *out);
/* Loaded and covering the whole log, else NULL (readers then full-scan). */
Idx *idx_ready(Arena *a, const Repo *r);
/* Writer-side (lock held): index the log bytes not yet covered; a damaged
 * or missing index becomes a full rebuild. */
bool idx_sync(Arena *a, const Repo *r, char *err, size_t errsz);

/* Whether idx describes r's history as a rebuild would: an entry for each
 * record where it lies, of its kind, and for a commit its file, op and the
 * file's commit before it; each file's last commit its head. One pass a
 * chunk at a time, so memory is a chunk's plus a slot per file. False with
 * the first difference in why ("index: ..."). */
bool idx_matches_log(Arena *a, const Repo *r, const Idx *idx, char *why,
                     size_t whysz);

/* ISO-8601 UTC -> epoch seconds; 0 when unparseable. */
uint64_t idx_epoch(const char *ts);

int32_t idx_file_id(const Idx *idx, const char *rel); /* -1 if unknown */
int64_t idx_find_commit(const Idx *idx, int64_t commit_no); /* entry; -1 */
/* Reads and decodes the record behind one entry. */
bool idx_fetch(Arena *a, const Repo *r, const Idx *idx, int64_t entry,
               Rec *out);

bool idx_write_heads(const Repo *r, const Idx *idx, char *err, size_t errsz);
/* Marks a snapshot taken at entry `at` and zeroes the file's budget. */
bool idx_reset_budget(const Repo *r, Idx *idx, int32_t fid, int64_t at,
                      char *err, size_t errsz);

#endif /* LAP_IDX_H */
