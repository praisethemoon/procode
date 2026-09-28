/* Repository discovery, state cache, shadow store, record append.
 *
 * Layout inside <root>/.lap/:
 *   log/         the history as chunk files (source of truth; hist.h)
 *   state.json   cache: counters, active session, last chain hash
 *   shadow/      last-committed content of every tracked file
 *   lock         exclusive lock taken by writing commands
 *
 * state.json is a cache of the log tail. If it disagrees with the log
 * (crash between append and state write, or a hand-edited log), repo_open
 * heals it by rescanning the log.
 */
#ifndef LAP_REPO_H
#define LAP_REPO_H

#include "hist.h"
#include "platform.h"
#include "rec.h"

typedef struct {
    Arena *a;
    char root[LAP_PATH_MAX];    /* absolute repo root, '/' separators */
    char lapdir[LAP_PATH_MAX];  /* <root>/.lap */
    Hist hist; /* this folder's history, as its chunks were at open */
    /* hist is another branch's history (a reader's --branch): the index,
     * the shadows and the state describe this folder's, not it. */
    bool foreign;

    int64_t next_commit;  /* next L<n> */
    int64_t next_session; /* next S<n> */
    char active_session[64]; /* "" when none */
    char active_session_msg[512];
    char last_hash[65];
    char cached_user[128]; /* git user.name, resolved once then persisted */

    PlatLock *lock; /* held by writing commands */

    /* What a missing shadow is replayed from (cmd_common.c): the index, or
     * without one the parsed log. Loaded on the first file that needs it and
     * reused for every other, while the log keeps shadow_log_size bytes;
     * loading it once per file made status grow with files x log size. */
    struct Idx *shadow_idx;
    RecLog *shadow_log;
    uint64_t shadow_log_size; /* history bytes when it was loaded */
    bool shadow_loaded;
} Repo;

/* Committer identity: $LAP_USER > cached git user.name > OS user. The git
 * lookup shells out once; the result rides state.json thereafter. */
const char *repo_user(Repo *r);

/* Rebuilds every derived cache (index, snapshots, shadows, state) from the
 * log. Requires the write lock; this is the crash-heal path and the body of
 * `lap rebuild`. */
bool repo_rebuild(Arena *a, Repo *r, char *err, size_t errsz);

/* Finds the repo from cwd upward and loads (healing if needed) the state.
 * for_write additionally takes the exclusive lock. Returns false with a
 * message in err when there is no repo or the state is unusable.
 */
bool repo_open(Arena *a, Repo *r, bool for_write, char *err, size_t errsz);
/* The error code of the last repo_open's failure: newer_history for a
 * writer refused because the history holds records of a type this lap does
 * not know, else no_repo. */
const char *repo_error_code(void);
/* The same for the repository whose root is root (absolute), found without
 * walking upward: how a branch reaches its parent folder. */
bool repo_open_at(Arena *a, Repo *r, const char *root, bool for_write,
                  char *err, size_t errsz);
/* A user-supplied path (relative to cwd or absolute) as an absolute,
 * normalized '/' path. */
bool repo_abspath(const char *user_path, char *out, size_t outsz);
void repo_close(Repo *r);

/* Creates a new repo in dir (absolute). Fails if one already exists there. */
bool repo_init(Arena *a, const char *dir, char *err, size_t errsz);
/* Writes lapdir/.gitignore, unless one is there: git keeps only log/ (and
 * the file itself), so nothing machine-local — a branch folder's lineage
 * and parent above all — reaches another folder through git. */
bool repo_write_gitignore(const char *lapdir);
/* Moves an older single-file log to chunks (hist_convert_legacy) and, when
 * it did, writes lapdir/.gitignore unless one is there, and says what to
 * commit to git. */
bool repo_convert_legacy(Arena *a, const char *lapdir, uint64_t limit,
                         bool *converted, char *err, size_t errsz);

/* Reads and parses this folder's whole history; messages name chunks and
 * lines ("main.000002.jsonl line 7"). */
bool repo_log_load(Arena *a, Repo *r, RecLog *out, char *err, size_t errsz);

/* Sets rec->prev (and rec->ts, to now, when it is NULL: an adopted record
 * keeps its own), encodes, appends to the log, refreshes r->last_hash in
 * memory. Does NOT persist state.json: callers finish their side effects
 * (shadow updates) first, then call repo_state_save — the log-then-shadow-
 * then-state order is what makes a crash at any point detectable, because
 * a stale state.json no longer matches the log tail and triggers a heal.
 */
bool repo_append(Repo *r, Rec *rec, char *err, size_t errsz);

/* Persists counters/active-session/last_hash to state.json (atomic). */
bool repo_state_save(Repo *r, char *err, size_t errsz);

/* Converts a user-supplied path (relative to cwd or absolute) into a
 * repo-relative '/' path. Fails when the path escapes the repo.
 */
bool repo_relpath(Repo *r, const char *user_path, char *out, size_t outsz,
                  char *err, size_t errsz);

/* Shadow store: content of a file as of its last commit. shadow_read reads
 * into a, the caller's arena. */
bool shadow_read(Arena *a, Repo *r, const char *rel, char **data,
                 size_t *len, bool *exists);
bool shadow_write(Repo *r, const char *rel, const void *data, size_t len);
bool shadow_remove(Repo *r, const char *rel);

/* True when data looks binary (NUL byte in the first 8 KB). */
bool looks_binary(const char *data, size_t len);

#endif /* LAP_REPO_H */
