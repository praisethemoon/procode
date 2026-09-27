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
/* The branch `key` (a name or an id) names, found in the registry reg or
 * among the branch chunks in this folder: its id, or NULL. */
const char *branch_find(Arena *a, const Repo *r, const Branches *reg,
                        const char *key);
/* True when folder `path` is still branch `id`: its .lap names it. */
bool branch_folder_is(Arena *a, const char *path, const char *id);
/* A reader's --branch: shows `name`'s history instead of this folder's —
 * a registered branch's folder when it is reachable, else its chunks
 * here; "main" in a branch folder is its parent's history to the base.
 * NULL, or this folder's own branch, changes nothing. False after
 * reporting unknown_branch. */
bool repo_view_branch(Arena *a, Repo *r, const char *name, bool json);

/* Post-write cache maintenance; failures warn, never fail the command. */
void caches_sync_warn(Arena *a, const Repo *r);

/* The one JSON shape for a commit, shared by log/search/show/rr: emits
 * the field list WITHOUT enclosing braces so callers can add their own. */
void json_commit(StrBuf *sb, const Rec *rec, const char *note);

/* Human one-liner + intent summary, shared by log and search. */
void print_commit_human(StrBuf *sb, const Rec *rec, bool with_region,
                        const char *note);

/* The first 7 hex digits of a commit's hash. */
#define SHORT_HASH_LEN 7
void short_hash(const Rec *rec, char out[SHORT_HASH_LEN + 1]);

/* Every line of text, each indented by `indent`. */
void sb_indented(StrBuf *sb, const char *indent, const char *text);

/* Reads -F's argument: the named file, or stdin for "-", trailing
 * whitespace trimmed. False with a reason in err. */
bool read_text_arg(Arena *a, const char *path, char **out, char *err,
                   size_t errsz);

/* Where a commit reference (SPEC §References) points in a loaded log: an
 * id ("L42"), or a hash or hash prefix of at least 7 hex digits, with or
 * without '#', in any case. Returns the record's index, or -1 with
 * *code = "unknown_ref" / "ambiguous_ref" and a sentence in err. */
int32_t ref_find(const RecLog *log, const char *ref, const char **code,
                 char *err, size_t errsz);
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
