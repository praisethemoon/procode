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
        !rec_log_load(a, repo->logpath, log, err, errsz))
        return false;
    if (!rec_replay_file(a, log, rec->file, (int32_t)entry, out, deleted)) {
        snprintf(err, errsz, "cannot reconstruct %s at %s", rec->file,
                 rec->id);
        return false;
    }
    return true;
}

int32_t cmd_show(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", "--full-file", NULL};
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    bool full_file = has_flag(argc, argv, NULL, "--full-file");
    const char *id = positional_arg(argc, argv, NULL, 0);
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
        if (!rec_log_load(a, repo.logpath, &log, err, sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            return LAP_EXIT_ERR;
        }
        const char *code;
        int32_t at = ref_find(&log, id, &code, err, sizeof err);
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
        sb_field(&sb, S_MUTED, rec->ts, 0);
        sb_putc(&sb, '\n');
        if (rec->user)
            sb_printf(&sb, "user: %s\n", rec->user);
        sb_puts(&sb, "file: ");
        sb_text(&sb, rec->file, strlen(rec->file));
        sb_printf(&sb, "  (%s)\n", rec->op);
        if (rec->forced)
            sb_puts(&sb, "forced: the message checks were skipped "
                         "(--force-message)\n");
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
