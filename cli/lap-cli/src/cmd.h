/* Command entry points and shared CLI helpers. */
#ifndef LAP_CMD_H
#define LAP_CMD_H

#include "branches.h"
#include "diff.h"
#include "ignore.h"
#include "json.h"
#include "msg.h"
#include "snap.h"
#include "tty.h"

int32_t cmd_init(Arena *a, int32_t argc, char **argv);
int32_t cmd_status(Arena *a, int32_t argc, char **argv);
int32_t cmd_commit(Arena *a, int32_t argc, char **argv);
int32_t cmd_log(Arena *a, int32_t argc, char **argv);
int32_t cmd_show(Arena *a, int32_t argc, char **argv);
int32_t cmd_search(Arena *a, int32_t argc, char **argv);
int32_t cmd_session(Arena *a, int32_t argc, char **argv);
int32_t cmd_verify(Arena *a, int32_t argc, char **argv);
int32_t cmd_rebuild(Arena *a, int32_t argc, char **argv);
int32_t cmd_rr(Arena *a, int32_t argc, char **argv);
int32_t cmd_branch(Arena *a, int32_t argc, char **argv);
int32_t cmd_merge(Arena *a, int32_t argc, char **argv);
int32_t cmd_amend(Arena *a, int32_t argc, char **argv);

/* ---- shared helpers (cmd_common.c) ----
 *
 * All the scanners understand the same grammar: flags listed in
 * value_flags (NULL-terminated, may be NULL) consume the next argument as
 * their value — so a message that *looks* like a flag ("-i --no-session")
 * is never misread as one — and a literal "--" ends flag parsing entirely
 * (everything after it is positional, allowing file names that start with
 * '-').
 */

/* Refuses, with unknown_flag, the first word before "--" that starts with
 * '-' and is none of the command's flags nor lap's colour flags. Every
 * command calls it first: a skipped flag would let its value pass for an
 * argument. */
bool flags_known(int32_t argc, char **argv, const char *const *value_flags,
                 const char *const *bool_flags);
bool has_flag(int32_t argc, char **argv, const char *const *value_flags,
              const char *flag);
const char *flag_value(int32_t argc, char **argv,
                       const char *const *value_flags, const char *flag);
const char *positional_arg(int32_t argc, char **argv,
                           const char *const *value_flags, int32_t index);
/* The value of whichever of two spellings is given ("-i" / "--intent"). */
const char *flag_value2(int32_t argc, char **argv,
                        const char *const *value_flags, const char *flag,
                        const char *alias);

void err_out(bool json_mode, const char *code, const char *fmt, ...);

/* Where branches exist, a commit or a session start says which line of
 * history it records to: `given` is --branch's value, or $LAP_BRANCH when
 * the flag is absent (NULL when neither). A folder that is a branch, or has
 * branches registered, needs it (branch_required); a name that is not this
 * folder's is wrong_branch, wherever it is given. Both messages name this
 * folder's branch. False after reporting the error. */
bool branch_check(Arena *a, const Repo *r, const char *given, bool json);
/* --branch's value, else $LAP_BRANCH, else NULL. */
const char *branch_given(int32_t argc, char **argv,
                         const char *const *value_flags);
/* The branches `key` (a name or an id) names, found in the registry reg or
 * among the branch chunks in this folder: their distinct ids in *ids, and
 * how many. An id names one branch; a name names one too, except where two
 * branches were given it before names were checked across nested
 * branches. */
int32_t branch_find_all(Arena *a, const Repo *r, const Branches *reg,
                        const char *key, const char ***ids);
/* The one branch key names: its id. NULL when it names none, or several —
 * then *several (when given) lists their ids, else it is set to NULL. */
const char *branch_find(Arena *a, const Repo *r, const Branches *reg,
                        const char *key, const char **several);
/* Whether a branch start may not use `name` from folder `from`: it is
 * taken wherever main can see it. Going up from `from` through each
 * folder's .lap/parent to the top, a folder's branches, nested ones
 * included, its branch chunks and the names its merges recorded. */
bool branch_name_used(Arena *a, const char *from, const char *name);
/* True when folder `path` is still branch `id`: its .lap names it. */
bool branch_folder_is(Arena *a, const char *path, const char *id);
/* A reader's --branch: shows `name`'s history instead of this folder's —
 * a registered branch's folder when it is reachable, else its chunks
 * here; "main" in a branch folder is its parent's history to the base.
 * NULL, or this folder's own branch, changes nothing. False after
 * reporting unknown_branch. */
bool repo_view_branch(Arena *a, Repo *r, const char *name, bool json);

/* A session named on a command line: "S<n>", this folder's own, or
 * "<branch>/S<n>", that branch's (its name or id). *sid is the id to look
 * for in r's history. For a branch's session r is switched to the branch's
 * history, as --branch does — unless lap merge adopted that session here,
 * in which case *sid is the adopted session's id in this folder's history
 * and *adopted is set. False after reporting unknown_branch or
 * unknown_session. */
bool session_resolve(Arena *a, Repo *r, const char *ref, const char **sid,
                     bool *adopted, bool json);
/* Where branch `lineage`'s own records start in log (its view): just after
 * its branch record; 0 for main (lineage NULL), whose records are all its
 * own. A session before that is inherited from the branch it started from. */
int32_t own_part_start(const RecLog *log, const char *lineage);
/* How a session is named to people and agents: "<branch>/S<n>" when the
 * branch it belongs to (a record's lineage label) is not main, else
 * "S<n>". Unique wherever it is copied to. */
const char *session_ref(Arena *a, const char *lineage, const char *sid);

/* One of a branch's own chunks, as lap merge reads it. */
typedef struct {
    char name[64];
    char *data; /* complete lines only */
    size_t len;
    bool write; /* not here as read: missing here, or a shorter copy */
} OwnChunk;
/* A branch's own chunks, each taken from this folder's copy (lapdir) when
 * there is one. Under git that copy is what git merge brought, so the
 * history lap merge adopts is the one whose code is here, and a file git
 * tracks is never rewritten. The branch folder (from; NULL when
 * unreachable) fills in only when fill is set — a folder without git, or
 * --copy-from-folder — with the chunks missing here. It extends a copy here
 * that is a byte prefix of its own only when extend is set (no git: nothing
 * else brings the rest); else that copy is the last chunk taken, and
 * *behind says the folder has more. Stops at the first chunk neither side
 * has. */
bool own_chunks(Arena *a, const char *lapdir, const char *from,
                const char *id, bool fill, bool extend, OwnChunk **out,
                int32_t *n, bool *behind, char *err, size_t errsz);
/* Where lap merge takes this folder's files when placing a branch's new
 * records (their hashes in newer): the index in log just before the
 * records an interrupted run of the merge appended (found by their from,
 * with its own merge records of run_branches, the chain's ids, among
 * them), when they are the last thing in log; else log's last record. */
int32_t merge_redo_point(const RecLog *log, const StrSet *newer,
                         const StrSet *run_branches);
/* Where the merge records an interrupted run of a chain merge wrote begin
 * in log: the trailing merge records, each of one of the n outer branches
 * (ids) at exactly the head the chain writes for it (heads). A chain
 * writes the merged branch's own record last, so these mean the run did
 * not finish, and its rerun reads them as its own, not as earlier merges.
 * log's count when there are none. */
int32_t merge_cut_start(const RecLog *log, const char *const *ids,
                        const char *const *heads, int32_t n);
/* Whether a merge that finds nothing to adopt (start, the first record of
 * the branch's stream not taken in yet, is count) still records itself:
 * when the branch (heads[0], its last merged head) was never merged — one
 * with no commits at all. unmerged[k] marks each branch of the chain with
 * no merge yet, which gets a merge record of its own. */
bool merge_nothing_yet(int32_t start, int32_t count,
                       const char *const *heads, int32_t n, bool *unmerged);
/* The records of a type this lap does not know among those a merge would
 * adopt (lof[i] >= 0: record i belongs to a branch of the chain; a record
 * of this folder's own part, -1, is not counted): how many, and the first
 * one's type in *type (NULL when none). */
int32_t merge_unknown_records(const RecLog *log, const int32_t *lof,
                              const char **type);

/* The files a history tracks: each file whose last commit in log is not a
 * delete, in the order they first appear. What the index knows, read from
 * the history when there is no index. */
size_t log_tracked_files(Arena *a, const RecLog *log, const char ***out);

/* Post-write cache maintenance; failures warn, never fail the command. */
void caches_sync_warn(Arena *a, const Repo *r);

/* The one JSON shape for a commit, shared by log/search/show/rr: emits
 * the field list WITHOUT enclosing braces so callers can add their own. */
void json_commit(StrBuf *sb, const Rec *rec, const char *note);

/* Which of a commit's texts a human listing shows under its line: the
 * intent alone (search, log --intent-only), the behavior alone (log
 * --behavior-only), or both, labelled (log). */
typedef enum { SHOW_INTENT, SHOW_BEHAVIOR, SHOW_BOTH } CommitText;

/* Human one-liner + the first line of the texts asked for, shared by log
 * and search. */
void print_commit_human(StrBuf *sb, const Rec *rec, bool with_region,
                        const char *note, CommitText show);

/* "(amended)", or "(amended <n> times)", for a commit lap amend corrected. */
const char *amend_marker(Arena *a, const Rec *rec);

/* The first 7 hex digits of a commit's hash. */
#define SHORT_HASH_LEN 7
void short_hash(const Rec *rec, char out[SHORT_HASH_LEN + 1]);

/* Every line of text, each indented by `indent`. */
void sb_indented(StrBuf *sb, const char *indent, const char *text);

/* Reads -F's argument: the named file, or stdin for "-", trailing
 * whitespace trimmed. False with a reason in err. */
bool read_text_arg(Arena *a, const char *path, char **out, char *err,
                   size_t errsz);

/* Intent and behavior from -i/-b, or from the sections of a -F file
 * (lap commit, lap amend). False after reporting the error. */
bool message_args(Arena *a, int32_t argc, char **argv,
                  const char *const *value_flags, bool json,
                  const char **intent, const char **behavior);

/* A new or deleted file has no pending edits to pick: the error code for an
 * --edit given to either ("bad_edit_index"), or a --lines given to a delete
 * ("bad_lines"), or NULL. --lines on a create picks its part (create_part). */
const char *whole_file_pick_error(const char *op, const char *edit_arg,
                                  const char *lines_arg);

/* The part of a new file a create records: lines a..b of work (1-based,
 * inclusive). It ends with a newline unless it ends the file, where it takes
 * the file's own state. False when a..b is not inside the file. */
bool create_part(Lines work, int32_t a, int32_t b, Lines *out);

/* Where a commit reference (SPEC §References) points in a loaded log: an
 * id ("L42"), or a hash or hash prefix of at least 7 hex digits, with or
 * without '#', in any case. Returns the record's index, or -1 with
 * *code = "unknown_ref" / "ambiguous_ref" and a sentence in err. */
int32_t ref_find(const RecLog *log, const char *ref, const char **code,
                 char *err, size_t errsz);
/* ref_find over every record: a hash names whatever record it is (a
 * session's start, an amendment), and "S<n>" names a session's start. For
 * a command that says what else a reference named (lap amend's
 * not_a_commit). */
int32_t ref_find_record(const RecLog *log, const char *ref,
                        const char **code, char *err, size_t errsz);
/* True when ref is spelled as a commit id rather than a hash. */
bool ref_is_id(const char *ref);


/* Renders one region for humans, e.g. "lines 10-14" / "lines 40-42 (deleted)"
 * / "line 7 (insertion)".
 */
void region_describe(const Region *r, char *out, size_t outsz);

/* Reads working + shadow state of one file and diffs them. */
typedef struct {
    bool work_exists;
    bool shadow_exists;
    bool binary;
    bool unreadable; /* file_diff_load failed: the working file may be there
                        but cannot be read (no permission, or too large) */
    Lines work;
    Lines shadow;
    Regions regions;
} FileDiff;

bool file_diff_load(Arena *a, Repo *r, const char *rel, FileDiff *out,
                    char *err, size_t errsz);

/* Writes a unified-diff-style render of one commit's region to sb. */
void render_commit_diff(StrBuf *sb, const Rec *rec);

/* One diff line: indent, then sign and content sharing a single style, so
 * the marker and the text it marks read as one thing. */
void sb_diff_line(StrBuf *sb, Style s, const char *indent, const char *sign,
                  Str text);

#endif /* LAP_CMD_H */
