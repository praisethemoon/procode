#include "cmd.h"

/* Reconstructs rec's file as of `entry`, seeded from snapshots when the
 * index can serve it and replayed from birth otherwise. Both routes must
 * agree; the fallback exists so a damaged cache costs speed, not output. */
static bool show_replay(Arena *a, Repo *repo, Idx *ix, const Rec *rec,
                        int64_t entry, RecLog *log, Lines *out, bool *deleted,
                        char *err, size_t errsz) {
    if (ix && snap_replay(a, repo, ix, rec->file, entry, out, deleted))
        return true;
    if (log->count == 0 &&
        !repo_log_load(a, repo, log, err, errsz))
        return false;
    if (!rec_replay_file(a, log, rec->file, (int32_t)entry, out, deleted)) {
        snprintf(err, errsz, "cannot reconstruct %s at %s", rec->file,
                 rec->id);
        return false;
    }
    return true;
}

int32_t cmd_show(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {"--branch", NULL};
    static const char *const bool_flags[] = {"--json", "--full-file", NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    bool full_file = has_flag(argc, argv, value_flags, "--full-file");
    const char *view = flag_value(argc, argv, value_flags, "--branch");
    const char *id = positional_arg(argc, argv, value_flags, 0);
    if (!id) {
        err_out(json, "usage", "usage: lap show <commit> [--full-file] "
                               "[--json]  (an id, a hash or a hash prefix)");
        return LAP_EXIT_ERR;
    }

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    if (!repo_view_branch(a, &repo, view, json))
        return LAP_EXIT_ERR;
    RecLog log;
    memset(&log, 0, sizeof log);
    Rec fetched;
    const Rec *rec = NULL;
    int64_t entry = -1;
    Idx *ix = idx_ready(a, &repo);
    if (ix && ref_is_id(id)) {
        entry = idx_find_commit(ix, strtoll(id + 1, NULL, 10));
        /* the index's id is a derived ordinal: confirm the record really is
         * the one asked for before trusting the shortcut */
        if (entry >= 0 && idx_fetch(a, &repo, ix, entry, &fetched) &&
            strcmp(fetched.id, id) == 0)
            rec = &fetched;
    }
    if (!rec) { /* no index, or it could not answer: scan */
        ix = NULL;
        if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            return LAP_EXIT_ERR;
        }
        const char *code;
        int32_t at = ref_find(&log, id, &code, err, sizeof err);
        /* A hash this history lacks may be another branch's, present here
         * as its chunks: an adopted commit's from link. */
        if (at < 0 && !view && !ref_is_id(id) &&
            strcmp(code, "unknown_ref") == 0) {
            const char **ids;
            int32_t n = hist_lineages(a, repo.lapdir, &ids);
            for (int32_t i = 0; i < n && at < 0; i++) {
                Hist other;
                char oerr[512];
                if (strcmp(ids[i], repo.hist.lineage) == 0 ||
                    !hist_open_lineage(a, repo.lapdir, ids[i], &other, oerr,
                                       sizeof oerr))
                    continue;
                Repo alt = repo;
                alt.hist = other;
                alt.foreign = true;
                RecLog olog;
                const char *ocode;
                if (!repo_log_load(a, &alt, &olog, oerr, sizeof oerr))
                    continue;
                int32_t oat = ref_find(&olog, id, &ocode, oerr, sizeof oerr);
                if (oat >= 0) {
                    repo = alt;
                    log = olog;
                    at = oat;
                }
            }
        }
        if (at < 0) {
            err_out(json, code, "%s", err);
            return LAP_EXIT_ERR;
        }
        rec = &log.v[at];
        entry = at;
    }

    Lines content;
    bool deleted = false;
    bool have_content = false;
    if (full_file) {
        have_content = show_replay(a, &repo, ix, rec, entry, &log, &content,
                                   &deleted, err, sizeof err);
        if (!have_content) {
            err_out(json, "replay_failed", "%s", err);
            return LAP_EXIT_ERR;
        }
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,");
        json_commit(&sb, rec, NULL);
        sb_puts(&sb, ",\"old_text\":[");
        for (int32_t i = 0; i < rec->old_n; i++) {
            if (i)
                sb_putc(&sb, ',');
            json_escape(&sb, rec->old_text[i].ptr, rec->old_text[i].len);
        }
        sb_puts(&sb, "],\"new_text\":[");
        for (int32_t i = 0; i < rec->new_n; i++) {
            if (i)
                sb_putc(&sb, ',');
            json_escape(&sb, rec->new_text[i].ptr, rec->new_text[i].len);
        }
        sb_printf(&sb, "],\"eof_nl\":%s", rec->eof_nl ? "true" : "false");
        if (have_content) {
            sb_printf(&sb, ",\"file_deleted\":%s,\"file_content\":",
                      deleted ? "true" : "false");
            size_t blen;
            char *bytes = join_lines(a, content, &blen);
            json_escape(&sb, bytes, blen);
        }
        sb_puts(&sb, "}");
        puts(sb_finish(&sb));
    } else {
        sb_puts(&sb, "commit ");
        sb_field(&sb, S_ID, rec->id, 0);
        sb_putc(&sb, ' ');
        sb_field(&sb, S_MUTED, rec->hash, 0);
        if (rec->session) {
            sb_puts(&sb, "  (session ");
            sb_field(&sb, S_SESSION, rec->session, 0);
            sb_putc(&sb, ')');
        }
        sb_puts(&sb, "\ndate: ");
        char when[40];
        plat_ts_local(rec->ts, true, when);
        sb_field(&sb, S_MUTED, when, 0);
        sb_putc(&sb, '\n');
        if (rec->user)
            sb_printf(&sb, "user: %s\n", rec->user);
        if (rec->lineage && strcmp(rec->lineage, LAP_MAIN_LINEAGE) != 0)
            sb_printf(&sb, "branch: %s\n", rec->lineage);
        sb_puts(&sb, "file: ");
        sb_text(&sb, rec->file, strlen(rec->file));
        sb_printf(&sb, "  (%s)\n", rec->op);
        if (rec->forced)
            sb_puts(&sb, "forced: the message checks were skipped "
                         "(--force-message)\n");
        if (rec->from)
            sb_printf(&sb, "from: #%.7s (adopted from a branch by lap "
                           "merge; the original is %s)\n",
                      rec->from, rec->from);
        sb_puts(&sb, "intent:\n");
        sb_indented(&sb, "  ", rec->intent);
        sb_puts(&sb, "behavior:\n");
        sb_indented(&sb, "  ", rec->behavior);
        sb_puts(&sb, "diff:\n");
        render_commit_diff(&sb, rec);
        if (have_content) {
            if (deleted) {
                sb_puts(&sb, "file after this commit: (deleted)\n");
            } else {
                sb_puts(&sb, "file after this commit:\n");
                for (int32_t i = 0; i < content.count; i++) {
                    sb_printf(&sb, "%5d | ", i + 1);
                    sb_text(&sb, content.lines[i].ptr,
                            content.lines[i].len);
                    sb_putc(&sb, '\n');
                }
            }
        }
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}
