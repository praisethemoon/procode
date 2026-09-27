/* Repository discovery, state cache, shadow store, record append.
 *
 * Layout inside <root>/.lap/:
 *   log.jsonl    append-only record log (source of truth)
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

#include "platform.h"
#include "rec.h"

typedef struct {
    Arena *a;
    char root[LAP_PATH_MAX];    /* absolute repo root, '/' separators */
    char lapdir[LAP_PATH_MAX];  /* <root>/.lap */
    char logpath[LAP_PATH_MAX]; /* <root>/.lap/log.jsonl */

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
    uint64_t shadow_log_size;
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
void repo_close(Repo *r);

/* Creates a new repo in dir (absolute). Fails if one already exists there. */
bool repo_init(Arena *a, const char *dir, char *err, size_t errsz);

/* Sets rec->prev/ts, encodes, appends to the log, refreshes r->last_hash in
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

/* Shadow store: content of a file as of its last commit. */
bool shadow_read(Repo *r, const char *rel, char **data, size_t *len,
                 bool *exists);
bool shadow_write(Repo *r, const char *rel, const void *data, size_t len);
bool shadow_remove(Repo *r, const char *rel);

/* True when data looks binary (NUL byte in the first 8 KB). */
bool looks_binary(const char *data, size_t len);

#endif /* LAP_REPO_H */
