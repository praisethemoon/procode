/* Command entry points and shared CLI helpers. */
#ifndef LAP_CMD_H
#define LAP_CMD_H

#include "diff.h"
#include "ignore.h"
#include "json.h"
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

/* ---- shared helpers (cmd_common.c) ----
 *
 * All three scanners understand the same grammar: flags listed in
 * value_flags (NULL-terminated, may be NULL) consume the next argument as
 * their value — so a message that *looks* like a flag ("-m --no-session")
 * is never misread as one — and a literal "--" ends flag parsing entirely
 * (everything after it is positional, allowing file names that start with
 * '-').
 */
bool has_flag(int32_t argc, char **argv, const char *const *value_flags,
              const char *flag);
const char *flag_value(int32_t argc, char **argv,
                       const char *const *value_flags, const char *flag);
const char *positional_arg(int32_t argc, char **argv,
                           const char *const *value_flags, int32_t index);

void err_out(bool json_mode, const char *code, const char *fmt, ...);

/* Post-write cache maintenance; failures warn, never fail the command. */
void caches_sync_warn(Arena *a, const Repo *r);

/* The one JSON shape for a commit, shared by log/search/show: emits the
 * field list WITHOUT enclosing braces so callers can add their own. */
void json_commit(StrBuf *sb, const Rec *rec, const char *note);

/* Human one-liner + message summary, shared by log and search. */
void print_commit_human(StrBuf *sb, const Rec *rec, bool with_region,
                        const char *note);

/* Resolves a message from -m "text" or -F <file> (-F - reads stdin).
 * Trailing whitespace/newlines are trimmed. Returns:
 *   1  message present -> *out set
 *   0  neither flag given -> *out NULL (caller emits its usage error)
 *  -1  error (both flags, unreadable file, empty message) -> err filled
 */
int32_t message_arg(Arena *a, int32_t argc, char **argv,
                    const char *const *value_flags, const char **out,
                    char *err, size_t errsz);


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
