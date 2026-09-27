/* Log records: encoding, decoding, chain hashing, log-file loading.
 *
 * The log is append-only JSONL, kept as chunk files under .lap/log/ (see
 * hist.h) and read as one stream. Every record carries
 * "prev": the SHA-256 of the previous record's exact serialized bytes
 * (the first record chains from 64 zeros). Tampering or truncation in the
 * middle of the file breaks the chain and is detected by `lap verify`.
 */
#ifndef LAP_REC_H
#define LAP_REC_H

#include "str.h"

#define LAP_HASH_ZERO                                                          \
    "0000000000000000000000000000000000000000000000000000000000000000"

typedef enum {
    REC_INIT,
    REC_COMMIT,
    REC_SESSION_START,
    REC_SESSION_END,
    REC_BRANCH, /* the first record of a branch's own lineage */
    REC_MERGE,  /* closes a lap merge: what of a branch was adopted */
    REC_AMEND,  /* a commit's intent and behavior, corrected */
    /* a type a newer lap wrote: in the chain (prev, ts, hash), otherwise
     * not understood; its type name is kept in name */
    REC_UNKNOWN
} RecType;

typedef struct {
    RecType type;
    const char *id;      /* commit: "L<n>", session: "S<n>" */
    const char *user;    /* commit + session_start; NULL in old logs.
                            Resolution: $LAP_USER > git user.name > OS user */
    const char *session; /* commit only; NULL when committed --no-session */
    const char *file;    /* commit only; repo-relative */
    const char *ts;      /* ISO-8601 UTC */
    const char *op;      /* commit only: "edit" | "create" | "delete" */
    const char *msg;      /* session_start: the session's purpose */
    const char *intent;   /* commit: why the edit exists */
    const char *behavior; /* commit: what this edit makes the code do */
    bool forced;          /* commit: written under --force-message */
    /* session_start only: a flat object, e.g. {"ticket":"T-12","n":1}, so a
     * session can be found by what it was for. Keys are identifiers; each
     * value is held as its JSON text ("\"T-12\"", "1", "true") so it is
     * written back exactly and compared as written. Every new session_start
     * carries the object, empty or not; lines from before metadata existed
     * have none and decode with meta_n == 0. */
    const char **meta_keys;
    const char **meta_vals;
    int32_t meta_n;
    /* branch only: id is the branch's lineage (12 hex digits), name what
     * people type, parent the lineage it started from, base the parent's
     * head then (also its prev), base_chunk the parent's chunk that base
     * ends. */
    const char *name;
    const char *parent;
    const char *base;
    int32_t base_chunk;
    /* commit, session_start, session_end: the hash of the branch record
     * this one adopted (lap merge); NULL for the folder's own work. */
    const char *from;
    /* merge only: branch is the adopted branch's id (name its name), head
     * the hash of its last record adopted, adopted and left the commits
     * placed and not, stopped the files a conflict stopped with the hash
     * of the first commit to each that was not placed, already the hashes
     * of branch commits whose change this folder had already made. */
    const char *branch;
    const char *head;
    int32_t adopted, left;
    const char **stopped_file;
    const char **stopped_at;
    int32_t stopped_n;
    const char **already;
    int32_t already_n;
    /* amend only: the hash of the commit whose intent and behavior (this
     * record's) replace what it said; from, when lap merge carried it over
     * from a branch, the branch's amend record */
    const char *of;
    const char *prev;    /* hex chain hash */
    int32_t old_start, old_lines, new_start, new_lines; /* commit only */
    Str *old_text;
    int32_t old_n;
    Str *new_text;
    int32_t new_n;
    bool eof_nl;     /* file ends with newline after this commit */
    int32_t version; /* init only */
    /* Not encoded: the branch whose chunk the record was read from, by
     * name ("main", or the branch's name), set by the readers that know
     * the chunks (repo_log_load, idx_fetch); NULL elsewhere. */
    const char *lineage;
    /* Not encoded: a commit's amendments, applied by readers (rec_amend_log,
     * idx_fetch) — intent and behavior then hold the latest text; amended
     * counts them, and earlier_* hold every text before it, oldest first
     * (the commit's own, then each replaced amendment's). */
    int32_t amended;
    const char **earlier_intent, **earlier_behavior;
    const char **earlier_user, **earlier_ts;
    int32_t earlier_n;
    const char *amend_user, *amend_ts; /* who wrote the latest text, when */
    char hash[65];   /* SHA-256 of raw, filled by decode/append */
    const char *raw; /* serialized line (no trailing newline) */
    size_t raw_len;
} Rec;

/* Serializes rec (canonical field order), fills rec->hash and rec->raw. */
char *rec_encode(Arena *a, Rec *rec, size_t *out_len);

/* Parses one line; returns false with a message in err on malformed input. */
bool rec_decode(Arena *a, const char *line, size_t len, Rec *out, char *err,
                size_t errsz);

typedef struct {
    Rec *v;
    int32_t count;
    /* Chain verification result (always computed). */
    bool chain_ok;
    int32_t chain_break_index; /* first bad record, -1 if chain_ok */
    uint64_t chain_break_off;  /* its byte offset in what was parsed */
    char chain_err[256];
    /* A crash mid-append leaves an unterminated final line. Readers drop it
     * (it was never acknowledged) and report it here; the next writing
     * command truncates it away.
     */
    bool torn_tail;
    uint64_t torn_bytes; /* bytes dropped from the tail */
    /* records of a type a newer lap wrote: how many, and the first's type */
    int32_t unknown_n;
    const char *unknown_type;
} RecLog;

/* Names a byte offset of the parsed data for people, e.g.
 * "main.000002.jsonl line 7". */
typedef void (*RecWhereFn)(const void *ctx, const char *data, uint64_t off,
                           char *out, size_t outsz);

/* Applies amendment am to commit c: the latest text replaces c's, which
 * goes to its earlier texts (room: how many amendments c has in all, for
 * the first allocation). */
void rec_amend_one(Arena *a, Rec *c, const Rec *am, int32_t room);
/* Applies every amendment in log to the commit it names, in log order, so
 * each commit holds its latest text; the amend records stay in the log but
 * are no history entries of their own. */
void rec_amend_log(Arena *a, RecLog *log);

/* Tells the user, once a command, that the history holds records of a type
 * a newer lap wrote (type NULL when the name is not at hand): readers skip
 * them and go on. */
void rec_note_newer(const char *type);

/* Parses a whole log held in data (records point into it). Returns false
 * only when a complete record is unparseable; a torn final line is
 * tolerated (see torn_tail) and a broken hash chain still loads (readers
 * stay usable on a damaged repo) and is reported through chain_ok. where
 * names positions in messages; NULL gives "log line N".
 */
bool rec_log_parse(Arena *a, const char *data, size_t len, RecWhereFn where,
                   const void *where_ctx, RecLog *out, char *err,
                   size_t errsz);

/* Replays commits [0..upto_index] (inclusive; may be -1 for "nothing") for
 * one file. Returns false if the file never appeared; *deleted reports a
 * final delete op.
 */
bool rec_replay_file(Arena *a, const RecLog *log, const char *rel,
                     int32_t upto_index, Lines *out, bool *deleted);

/* Applies one commit record to a file state. The single definition of what
 * a record means; every replay path goes through it. */
void rec_apply(Arena *a, Lines *cur, const Rec *rec);

/* "S12" -> 12; 0 for anything that is not a session id (NULL, empty,
 * trailing garbage). Both the index and the scan paths parse session
 * filters with this, so they can never disagree. */
uint32_t rec_session_no(const char *id);

/* The JSON text of one metadata value on a session_start record, or NULL. */
const char *rec_meta(const Rec *rec, const char *key);

/* Emits `,"meta":{...}` for a session_start record — `{}` when it has none —
 * so JSON readers always find the key. */
void rec_meta_json(StrBuf *sb, const Rec *rec);

/* `key=value` from a command line to a metadata pair. The key must be an
 * identifier ([A-Za-z_][A-Za-z0-9_]*). The value becomes a JSON number when it
 * is one, true/false when it is one of those, and a string otherwise; *val is
 * its JSON text. Returns false with a message in err. */
bool rec_meta_parse(Arena *a, const char *kv, const char **key,
                    const char **val, char *err, size_t errsz);

#endif /* LAP_REC_H */
