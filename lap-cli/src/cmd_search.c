#include "cmd.h"

/* lap search — interrogates the history:
 *   --file F --line N     which commit last touched current line N (blame)
 *   --text STR            commits whose added/removed lines contain STR
 *   --added / --removed   narrow --text to one side
 *   --msg STR             commits whose message contains STR
 *   --session S / --since TS / --until TS / --limit N
 * Criteria AND together. Blame walks the file's index chain when one is
 * available. For listings the index pays off only when it can PREFILTER on
 * entry fields (file, session, time); a bare --text/--msg query has to read
 * every record body, which a sequential scan does far faster than one seek
 * per record.
 */

typedef struct {
    const char *text, *msg;
    bool only_added, only_removed;
} Match;

/* Body predicates; fills note with the matched line for the output. */
static bool rec_matches(const Rec *rec, const Match *m, char *note,
                        size_t notesz) {
    note[0] = '\0';
    if (m->msg && str_find(str_c(rec->msg), str_c(m->msg)) < 0)
        return false;
    if (!m->text)
        return true;
    Str needle = str_c(m->text);
    if (!m->only_removed) {
        for (int32_t k = 0; k < rec->new_n; k++) {
            if (str_find(rec->new_text[k], needle) >= 0) {
                snprintf(note, notesz, "added line %d: %.*s",
                         rec->new_start + k,
                         (int)(rec->new_text[k].len > 120
                                   ? 120
                                   : rec->new_text[k].len),
                         rec->new_text[k].ptr);
                return true;
            }
        }
    }
    if (!m->only_added) {
        for (int32_t k = 0; k < rec->old_n; k++) {
            if (str_find(rec->old_text[k], needle) >= 0) {
                snprintf(note, notesz, "removed line %d: %.*s",
                         rec->old_start + k,
                         (int)(rec->old_text[k].len > 120
                                   ? 120
                                   : rec->old_text[k].len),
                         rec->old_text[k].ptr);
                return true;
            }
        }
    }
    return false;
}

static void emit(StrBuf *sb, const Rec *rec, bool json, int64_t printed,
                 const char *note) {
    if (json) {
        if (printed)
            sb_putc(sb, ',');
        sb_putc(sb, '{');
        json_commit(sb, rec, note);
        sb_putc(sb, '}');
    } else {
        print_commit_human(sb, rec, false, note);
    }
}

/* Backward walk over the file's records; returns the index of the record
 * that last touched `line` (committed coordinates), or -1. */
static int32_t blame_line(const RecLog *log, const char *rel, int32_t line) {
    for (int32_t i = log->count - 1; i >= 0; i--) {
        const Rec *rec = &log->v[i];
        if (rec->type != REC_COMMIT || strcmp(rec->file, rel) != 0)
            continue;
        if (strcmp(rec->op, "delete") == 0)
            return -1;
        if (line >= rec->new_start && line < rec->new_start + rec->new_lines)
            return i;
        if (line >= rec->new_start + rec->new_lines)
            line -= rec->new_lines - rec->old_lines;
        if (strcmp(rec->op, "create") == 0)
            return -1;
    }
    return -1;
}

/* Same walk over the index chain: no record bodies read until the hit. */
static int64_t blame_chain(const Idx *ix, const char *rel, int32_t line) {
    int32_t fid = idx_file_id(ix, rel);
    for (int64_t e = fid >= 0 ? ix->heads[fid].head : -1; e >= 0;
         e = ix->v[e].prev_same_file) {
        const IdxEntry *en = &ix->v[e];
        if (en->op == IDX_OP_DELETE)
            return -1;
        if (line >= (int32_t)en->new_start &&
            line < (int32_t)(en->new_start + en->new_lines))
            return e;
        if (line >= (int32_t)(en->new_start + en->new_lines))
            line -= (int32_t)en->new_lines - (int32_t)en->old_lines;
        if (en->op == IDX_OP_CREATE)
            return -1;
    }
    return -1;
}

static void print_message(StrBuf *sb, const char *msg) {
    while (*msg) {
        const char *nl = strchr(msg, '\n');
        size_t len = nl ? (size_t)(nl - msg) : strlen(msg);
        sb_puts(sb, "  ");
        sb_putn(sb, msg, len);
        sb_putc(sb, '\n');
        if (!nl)
            break;
        msg = nl + 1;
    }
}

int32_t cmd_search(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {
        "--file", "--line", "--text", "--msg", "--session",
        "--since", "--until", "--limit", NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    const char *f_file = flag_value(argc, argv, value_flags, "--file");
    const char *f_line = flag_value(argc, argv, value_flags, "--line");
    const char *f_session = flag_value(argc, argv, value_flags, "--session");
    const char *f_since = flag_value(argc, argv, value_flags, "--since");
    const char *f_until = flag_value(argc, argv, value_flags, "--until");
    const char *f_limit = flag_value(argc, argv, value_flags, "--limit");
    Match m;
    m.text = flag_value(argc, argv, value_flags, "--text");
    m.msg = flag_value(argc, argv, value_flags, "--msg");
    m.only_added = has_flag(argc, argv, value_flags, "--added");
    m.only_removed = has_flag(argc, argv, value_flags, "--removed");
    int64_t limit = f_limit ? strtoll(f_limit, NULL, 10) : -1;

    if (!f_line && !m.text && !m.msg && !f_session && !f_since && !f_until &&
        !f_file) {
        err_out(json, "usage",
                "usage: lap search [--file F [--line N]] [--text STR "
                "[--added|--removed]] [--msg STR] [--session S] [--since TS] "
                "[--until TS] [--limit N] [--json]");
        return LAP_EXIT_ERR;
    }
    if (f_line && !f_file) {
        err_out(json, "usage", "--line requires --file");
        return LAP_EXIT_ERR;
    }

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    char rel[LAP_PATH_MAX] = "";
    if (f_file && !repo_relpath(&repo, f_file, rel, sizeof rel, err,
                                sizeof err)) {
        err_out(json, "bad_path", "%s", err);
        return LAP_EXIT_ERR;
    }
    uint32_t sess = rec_session_no(f_session);
    bool none = f_session && !sess;
    Idx *ix = idx_ready(a, &repo);

    /* ---- blame mode ---- */
    if (f_line) {
        int32_t line = (int32_t)strtol(f_line, NULL, 10);
        if (line < 1) {
            err_out(json, "bad_line", "--line expects a 1-based line number");
            return LAP_EXIT_ERR;
        }
        FileDiff fd;
        if (!file_diff_load(a, &repo, rel, &fd, err, sizeof err)) {
            err_out(json, "read_failed", "%s", err);
            return LAP_EXIT_ERR;
        }
        if (fd.work_exists && line > fd.work.count) {
            err_out(json, "bad_line", "%s has only %d lines", rel,
                    fd.work.count);
            return LAP_EXIT_ERR;
        }
        bool pending = fd.work_exists && !fd.shadow_exists;
        int32_t committed_line = line;
        if (fd.work_exists && fd.shadow_exists) {
            for (int32_t i = fd.regions.count - 1; i >= 0 && !pending; i--) {
                const Region *rg = &fd.regions.v[i];
                if (line >= rg->new_start &&
                    line < rg->new_start + rg->new_lines)
                    pending = true;
                else if (line >= rg->new_start + rg->new_lines)
                    committed_line -= rg->new_lines - rg->old_lines;
            }
        }
        if (pending) {
            if (json) {
                StrBuf psb;
                sb_init(&psb, a);
                sb_puts(&psb, "{\"ok\":true,\"pending\":true,\"file\":");
                json_escape_c(&psb, rel);
                sb_printf(&psb, ",\"line\":%d}", line);
                puts(sb_finish(&psb));
            } else {
                printf("line %d of %s is an uncommitted pending edit "
                       "(commit it to give it history)\n",
                       line, rel);
            }
            return LAP_EXIT_OK;
        }

        Rec fetched;
        const Rec *rec = NULL;
        if (ix) {
            int64_t e = blame_chain(ix, rel, committed_line);
            if (e >= 0 && idx_fetch(a, &repo, ix, e, &fetched))
                rec = &fetched;
        }
        if (!rec) { /* no index, or it could not answer: scan */
            RecLog log;
            if (!rec_log_load(a, repo.logpath, &log, err, sizeof err)) {
                err_out(json, "log_unreadable", "%s", err);
                return LAP_EXIT_ERR;
            }
            int32_t hit = blame_line(&log, rel, committed_line);
            if (hit >= 0)
                rec = &log.v[hit];
        }
        if (!rec) {
            err_out(json, "not_found",
                    "no commit in history covers line %d of %s", line, rel);
            return LAP_EXIT_ERR;
        }
        StrBuf sb;
        sb_init(&sb, a);
        if (json) {
            sb_puts(&sb, "{\"ok\":true,\"pending\":false,\"commit\":{");
            json_commit(&sb, rec, NULL);
            sb_puts(&sb, "}}");
            puts(sb_finish(&sb));
        } else {
            sb_printf(&sb, "line %d of %s was last touched by %s (%s)\n",
                      line, rel, rec->id, rec->ts);
            if (rec->session)
                sb_printf(&sb, "session: %s\n", rec->session);
            sb_puts(&sb, "message:\n");
            print_message(&sb, rec->msg);
            fputs(sb_finish(&sb), stdout);
        }
        return LAP_EXIT_OK;
    }

    /* ---- filter mode ---- */
    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"commits\":[");
    int64_t printed = 0;
    char note[256];
    uint64_t since = f_since ? idx_epoch(f_since) : 0;
    uint64_t until = f_until ? idx_epoch(f_until) : 0;

    /* The index earns its keep only when it can reject entries without
     * reading their bodies. */
    int32_t fid = ix && rel[0] ? idx_file_id(ix, rel) : -1;
    bool prefilterable = ix && (rel[0] || sess || since || until);
    bool served = false;
    if (!none && prefilterable && !(rel[0] && fid < 0)) {
        served = true;
        for (int64_t e = (int64_t)ix->h.count - 1; e >= 0; e--) {
            const IdxEntry *en = &ix->v[e];
            if (en->kind != IDX_COMMIT ||
                (sess && en->session != sess) ||
                (fid >= 0 && en->file_id != (uint32_t)fid) ||
                (since && en->ts < since) || (until && en->ts > until))
                continue;
            if (limit >= 0 && printed >= limit)
                break;
            Rec rec;
            if (!idx_fetch(a, &repo, ix, e, &rec)) {
                served = false; /* cache unusable: redo by scanning */
                break;
            }
            if (!rec_matches(&rec, &m, note, sizeof note))
                continue;
            emit(&sb, &rec, json, printed++, note[0] ? note : NULL);
        }
    }
    if (!none && !served && !(rel[0] && fid < 0 && ix)) {
        printed = 0;
        sb_init(&sb, a); /* discard any partial index output */
        if (json)
            sb_puts(&sb, "{\"ok\":true,\"commits\":[");
        RecLog log;
        if (!rec_log_load(a, repo.logpath, &log, err, sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            return LAP_EXIT_ERR;
        }
        for (int32_t i = log.count - 1; i >= 0; i--) {
            const Rec *rec = &log.v[i];
            if (rec->type != REC_COMMIT ||
                (rel[0] && strcmp(rec->file, rel) != 0) ||
                (sess && rec_session_no(rec->session) != sess) ||
                (f_since && strcmp(rec->ts, f_since) < 0) ||
                (f_until && strcmp(rec->ts, f_until) > 0))
                continue;
            if (!rec_matches(rec, &m, note, sizeof note))
                continue;
            if (limit >= 0 && printed >= limit)
                break;
            emit(&sb, rec, json, printed++, note[0] ? note : NULL);
        }
    }

    if (json) {
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        if (!printed)
            sb_puts(&sb, "no matching commits\n");
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}
