#include "cmd.h"

/* lap rr — a review request: what a run of work changed, and why.
 *
 * Two halves, and both matter. The TRAJECTORY is the ordered reasoning:
 * every commit's intent and behavior, in the order the work happened. The
 * NET CHANGE is what a reviewer of the result would see: each touched file
 * replayed to just before the range and again at its end, then diffed — so
 * edits that cancelled out show as nothing, and ten commits to one function
 * show as one coherent change.
 *
 * It reads; it never writes. Asking twice costs nothing and changes
 * nothing.
 */

typedef struct {
    int32_t first, last; /* indices into the log, inclusive */
    const char *title;   /* the session's purpose, when there is one */
    const char *label;
} Range;

static bool range_from_session(const RecLog *log, uint32_t sess, Range *out) {
    out->first = -1;
    out->last = -1;
    for (int32_t i = 0; i < log->count; i++) {
        const Rec *rec = &log->v[i];
        if (rec->type == REC_SESSION_START && rec_session_no(rec->id) == sess)
            out->title = rec->msg;
        if (rec->type != REC_COMMIT || rec_session_no(rec->session) != sess)
            continue;
        if (out->first < 0)
            out->first = i;
        out->last = i;
    }
    return out->first >= 0;
}

/* Both ends are commit references; a bad one is reported here. */
static bool range_from_refs(const RecLog *log, const char *from,
                            const char *to, bool json, Range *out,
                            bool *reported) {
    const char *code;
    char err[512];
    out->first = ref_find(log, from, &code, err, sizeof err);
    if (out->first >= 0)
        out->last = ref_find(log, to, &code, err, sizeof err);
    if (out->first < 0 || out->last < 0) {
        err_out(json, code, "%s", err);
        *reported = true;
        return false;
    }
    return out->first <= out->last;
}

/* Renders the net change of one file as a unified-style diff. */
static void render_net(StrBuf *sb, Arena *a, Lines before, Lines after,
                       int32_t *plus, int32_t *minus) {
    Regions rg = diff_lines(a, before, after);
    *plus = 0;
    *minus = 0;
    for (int32_t i = 0; i < rg.count; i++) {
        Region *g = &rg.v[i];
        *minus += g->old_lines;
        *plus += g->new_lines;
        sb_printf(sb, "    %s@@ -%d,%d +%d,%d @@%s\n", sgr(S_HUNK),
                  g->old_start, g->old_lines, g->new_start, g->new_lines,
                  sgr_off());
        for (int32_t k = 0; k < g->old_lines; k++) {
            int32_t at = g->old_start - 1 + k;
            if (at >= 0 && at < before.count)
                sb_diff_line(sb, S_REMOVED, "    ", "- ", before.lines[at]);
        }
        for (int32_t k = 0; k < g->new_lines; k++) {
            int32_t at = g->new_start - 1 + k;
            if (at >= 0 && at < after.count)
                sb_diff_line(sb, S_ADDED, "    ", "+ ", after.lines[at]);
        }
    }
}

int32_t cmd_rr(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {"--branch", NULL};
    static const char *const bool_flags[] = {"--json", "--no-diff", NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    bool no_diff = has_flag(argc, argv, value_flags, "--no-diff");
    const char *view = flag_value(argc, argv, value_flags, "--branch");
    const char *pos0 = positional_arg(argc, argv, value_flags, 0);
    const char *pos1 = positional_arg(argc, argv, value_flags, 1);

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    if (!repo_view_branch(a, &repo, view, json))
        return LAP_EXIT_ERR;
    /* a session named <branch>/S<n> reads that branch, or the session lap
     * merge adopted from it here */
    bool adopted = false;
    const char *named = pos0;
    if (pos0 && !pos1 &&
        !session_resolve(a, &repo, pos0, &pos0, &adopted, json))
        return LAP_EXIT_ERR;
    RecLog log;
    if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }

    Range rng;
    memset(&rng, 0, sizeof rng);
    rng.title = "";
    bool ok = false;
    char label[160];

    if (pos0 && pos1) {
        bool reported = false;
        ok = range_from_refs(&log, pos0, pos1, json, &rng, &reported);
        if (reported)
            return LAP_EXIT_ERR;
        snprintf(label, sizeof label, "%s..%s", pos0, pos1);
    } else {
        uint32_t sess = rec_session_no(pos0);
        if (!sess) { /* no target given: review the most recent session */
            for (int32_t i = log.count - 1; i >= 0 && !sess; i--) {
                if (log.v[i].type == REC_SESSION_START)
                    sess = rec_session_no(log.v[i].id);
            }
        }
        if (!sess) {
            err_out(json, "no_target",
                    "nothing to review; name a session or a range "
                    "(<from> <to>)");
            return LAP_EXIT_ERR;
        }
        ok = range_from_session(&log, sess, &rng);
        char sid[16];
        snprintf(sid, sizeof sid, "S%u", sess);
        const char *ref = session_ref(
            a, repo.hist.parent[0] ? repo.hist.name : LAP_MAIN_LINEAGE, sid);
        if (adopted)
            snprintf(label, sizeof label, "%s (adopted from %s)", ref, named);
        else
            snprintf(label, sizeof label, "%s", ref);
    }
    if (!ok) {
        err_out(json, "empty_range", "%s holds no commits", label);
        return LAP_EXIT_ERR;
    }
    rng.label = label;

    /* every file the range touches, in first-touch order */
    StrSet seen;
    strset_init(&seen, a);
    char **files = NULL;
    size_t nfiles = 0, fcap = 0;
    int32_t ncommits = 0;
    for (int32_t i = rng.first; i <= rng.last; i++) {
        if (log.v[i].type != REC_COMMIT)
            continue;
        ncommits++;
        if (strset_add(&seen, log.v[i].file)) {
            ARENA_GROW(a, files, nfiles, fcap, char *);
            files[nfiles++] = arena_strdup(a, log.v[i].file);
        }
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"range\":");
        json_escape_c(&sb, label);
        sb_puts(&sb, ",\"purpose\":");
        json_escape_c(&sb, rng.title ? rng.title : "");
        sb_printf(&sb, ",\"commits\":%d,\"from\":\"%s\",\"to\":\"%s\"",
                  ncommits, log.v[rng.first].ts, log.v[rng.last].ts);
        sb_puts(&sb, ",\"trajectory\":[");
    } else {
        sb_puts(&sb, "review ");
        sb_field(&sb, S_ACTIVE, label, 0);
        if (rng.title && rng.title[0]) {
            const char *nl = strchr(rng.title, '\n');
            int32_t tl = nl ? (int32_t)(nl - rng.title)
                            : (int32_t)strlen(rng.title);
            sb_puts(&sb, " — ");
            sb_text(&sb, rng.title, (size_t)tl);
        }
        char from[40], to[40], span[96];
        plat_ts_local(log.v[rng.first].ts, false, from);
        plat_ts_local(log.v[rng.last].ts, false, to);
        snprintf(span, sizeof span, "%s → %s", from, to);
        sb_printf(&sb, "\n  %d commit%s · ", ncommits,
                  ncommits == 1 ? "" : "s");
        sb_field(&sb, S_MUTED, span, 0);
        sb_puts(&sb, "\n\ntrajectory:\n");
    }

    /* One path width for both halves, so each reads as a column of its
     * own. The halves are indented differently and do not line up with
     * each other; sharing the width is what keeps either one straight. */
    int32_t pathw = 0;
    for (size_t f = 0; f < nfiles; f++) {
        int32_t n = (int32_t)strlen(files[f]);
        if (n > pathw)
            pathw = n;
    }
    /* One deep path must not tax every other row with its width; past the
     * cap a path overflows its own line instead. */
    if (pathw > 40)
        pathw = 40;

    /* Humans read the trajectory by goal: a run of commits sharing an
     * intent prints it once, then each commit's own behavior. JSON stays
     * one entry per commit and leaves the grouping to its reader. */
    int32_t printed = 0;
    const char *heading = NULL;
    for (int32_t i = rng.first; i <= rng.last; i++) {
        const Rec *rec = &log.v[i];
        if (rec->type != REC_COMMIT)
            continue;
        if (json) {
            if (printed)
                sb_putc(&sb, ',');
            sb_putc(&sb, '{');
            json_commit(&sb, rec, NULL);
            sb_putc(&sb, '}');
        } else {
            if (!heading || strcmp(heading, rec->intent) != 0) {
                heading = rec->intent;
                if (printed)
                    sb_putc(&sb, '\n');
                sb_indented(&sb, "  ", rec->intent);
            }
            char sh[SHORT_HASH_LEN + 1];
            short_hash(rec, sh);
            const char *nl = strchr(rec->behavior, '\n');
            int32_t blen = nl ? (int32_t)(nl - rec->behavior)
                              : (int32_t)strlen(rec->behavior);
            sb_puts(&sb, "    ");
            sb_field(&sb, S_ID, rec->id, 6);
            sb_putc(&sb, ' ');
            sb_field(&sb, S_MUTED, sh, 0);
            sb_puts(&sb, "  ");
            sb_pad_text(&sb, rec->file, pathw);
            sb_puts(&sb, "  ");
            sb_text(&sb, rec->behavior, (size_t)blen);
            if (rec->forced) {
                sb_putc(&sb, ' ');
                sb_field(&sb, S_MUTED, "(forced)", 0);
            }
            if (rec->amended) {
                sb_putc(&sb, ' ');
                sb_field(&sb, S_MUTED, amend_marker(a, rec), 0);
            }
            if (rec->from) {
                char from[32];
                snprintf(from, sizeof from, "(from #%.7s)", rec->from);
                sb_putc(&sb, ' ');
                sb_field(&sb, S_MUTED, from, 0);
            }
            sb_putc(&sb, '\n');
        }
        printed++;
    }

    if (json)
        sb_puts(&sb, "],\"files\":[");
    else
        sb_puts(&sb, "\nnet change:\n");

    for (size_t f = 0; f < nfiles; f++) {
        Lines before, after;
        bool d1 = false, d2 = false;
        bool has_before = rec_replay_file(a, &log, files[f], rng.first - 1,
                                          &before, &d1);
        bool has_after =
            rec_replay_file(a, &log, files[f], rng.last, &after, &d2);
        if (!has_before || d1) {
            before.lines = NULL;
            before.count = 0;
            before.eof_nl = true;
        }
        if (!has_after || d2) {
            after.lines = NULL;
            after.count = 0;
            after.eof_nl = true;
        }
        /* always measure: --no-diff drops the hunks, not the summary */
        StrBuf body;
        sb_init(&body, a);
        int32_t plus = 0, minus = 0;
        render_net(&body, a, before, after, &plus, &minus);

        if (json) {
            if (f)
                sb_putc(&sb, ',');
            sb_puts(&sb, "{\"path\":");
            json_escape_c(&sb, files[f]);
            sb_printf(&sb, ",\"added\":%d,\"removed\":%d,\"deleted\":%s",
                      plus, minus, d2 ? "true" : "false");
            /* --no-diff means no diff, in either output shape */
            if (!no_diff) {
                sb_puts(&sb, ",\"diff\":");
                json_escape(&sb, body.data ? body.data : "", body.len);
            }
            sb_putc(&sb, '}');
        } else {
            sb_puts(&sb, "  ");
            sb_pad_text(&sb, files[f], pathw);
            sb_printf(&sb, "  %s+%d%s %s-%d%s", sgr(S_ADDED), plus,
                      sgr_off(), sgr(S_REMOVED), minus, sgr_off());
            if (d2) {
                sb_puts(&sb, "  ");
                sb_field(&sb, S_MUTED, "(deleted)", 0);
            }
            sb_putc(&sb, '\n');
            if (!no_diff && plus + minus == 0) {
                sb_puts(&sb, "    ");
                sb_field(&sb, S_MUTED,
                         "(no net change — edits cancelled out)", 0);
                sb_putc(&sb, '\n');
            }
            if (!no_diff)
                sb_putn(&sb, body.data ? body.data : "", body.len);
        }
    }

    if (json) {
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}
