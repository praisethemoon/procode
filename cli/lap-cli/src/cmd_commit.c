#include "cmd.h"

/* Parses "A-B" or "A" (single line) into a 1-based inclusive range. */
static bool parse_lines_arg(const char *s, int32_t *out_a, int32_t *out_b) {
    char *end = NULL;
    long a = strtol(s, &end, 10);
    if (end == s || a < 1)
        return false;
    long b = a;
    if (*end == '-') {
        const char *bs = end + 1;
        b = strtol(bs, &end, 10);
        if (end == bs || b < a)
            return false;
    }
    if (*end != '\0')
        return false;
    *out_a = (int32_t)a;
    *out_b = (int32_t)b;
    return true;
}

/* Matches --lines A-B against a region: the region's range in the current
 * file, or (for pure deletions, which occupy no current lines) its range in
 * the last-committed file.
 */
static bool region_matches_lines(const Region *r, int32_t a, int32_t b) {
    if (r->new_lines > 0) {
        if (a == r->new_start && b == r->new_start + r->new_lines - 1)
            return true;
    }
    if (r->old_lines > 0 && r->new_lines == 0) {
        if (a == r->old_start && b == r->old_start + r->old_lines - 1)
            return true;
    }
    return false;
}

static void print_multi_edit_error(Arena *a, bool json, const char *rel,
                                   const Regions *rg) {
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_puts(&sb, "{\"ok\":false,\"error\":\"multiple_edits\",\"file\":");
        json_escape_c(&sb, rel);
        sb_puts(&sb, ",\"edits\":[");
        for (int32_t i = 0; i < rg->count; i++) {
            Region *r = &rg->v[i];
            if (i)
                sb_putc(&sb, ',');
            sb_printf(&sb,
                      "{\"index\":%d,\"old_start\":%d,\"old_lines\":%d,"
                      "\"new_start\":%d,\"new_lines\":%d}",
                      i + 1, r->old_start, r->old_lines, r->new_start,
                      r->new_lines);
        }
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        fprintf(stderr, "error: %d separate edits detected in %s\n",
                rg->count, rel);
        for (int32_t i = 0; i < rg->count; i++) {
            char desc[128];
            region_describe(&rg->v[i], desc, sizeof desc);
            fprintf(stderr, "  [%d] %s\n", i + 1, desc);
        }
        fprintf(stderr,
                "a commit is one edit; pick one:\n"
                "  lap commit %s -i \"...\" -b \"...\" --edit <n>\n"
                "  lap commit %s -i \"...\" -b \"...\" --lines <start>-<end>\n",
                rel, rel);
    }
}

/* The behavior of the session's most recent commit, which the next one must
 * not repeat; NULL when the session has none yet. */
static const char *session_last_behavior(Arena *a, Repo *repo,
                                         const char *session) {
    uint32_t want = rec_session_no(session);
    Idx *ix = idx_ready(a, repo);
    if (ix) {
        for (int64_t e = (int64_t)ix->h.count - 1; e >= 0; e--) {
            const IdxEntry *en = &ix->v[e];
            if (en->kind == IDX_SESSION_START && en->session == want)
                return NULL;
            if (en->kind != IDX_COMMIT || en->session != want)
                continue;
            Rec rec;
            if (idx_fetch(a, repo, ix, e, &rec))
                return rec.behavior;
            break; /* the index could not answer: scan */
        }
    }
    RecLog log;
    char err[256];
    if (!repo_log_load(a, repo, &log, err, sizeof err))
        return NULL;
    for (int32_t i = log.count - 1; i >= 0; i--) {
        const Rec *rec = &log.v[i];
        if (rec->type == REC_SESSION_START && rec_session_no(rec->id) == want)
            return NULL;
        if (rec->type == REC_COMMIT && rec_session_no(rec->session) == want)
            return rec->behavior;
    }
    return NULL;
}

/* What a dry run would record. The JSON is the record as it would be
 * written, less `prev`: the chain link and the hash exist only once it is. */
static void print_dry_run(Arena *a, bool json, Rec *rec, const char *rel) {
    char ts[32];
    plat_timestamp(ts);
    rec->ts = ts;
    rec->prev = "";
    if (json) {
        size_t len;
        char *line = rec_encode(a, rec, &len);
        static const char tail[] = ",\"prev\":\"\"}";
        size_t tl = sizeof tail - 1;
        if (len >= tl && strcmp(line + len - tl, tail) == 0)
            memcpy(line + len - tl, "}", 2);
        printf("{\"ok\":true,\"dry_run\":true,\"record\":%s}\n", line);
        return;
    }
    Region shown = {rec->old_start, rec->old_lines, rec->new_start,
                    rec->new_lines};
    char desc[128];
    region_describe(&shown, desc, sizeof desc);
    StrBuf sb;
    sb_init(&sb, a);
    sb_puts(&sb, "dry run, nothing written: would record ");
    sb_field(&sb, S_ID, rec->id, 0);
    sb_puts(&sb, " in ");
    sb_field(&sb, S_SESSION, rec->session ? rec->session : "(no session)", 0);
    sb_printf(&sb, "\n  %s  ", rec->op);
    sb_text(&sb, rel, strlen(rel));
    sb_puts(&sb, "  ");
    sb_field(&sb, S_MUTED, desc, 0);
    if (rec->forced)
        sb_puts(&sb, "  (forced)");
    sb_puts(&sb, "\nintent:\n");
    sb_indented(&sb, "  ", rec->intent);
    sb_puts(&sb, "behavior:\n");
    sb_indented(&sb, "  ", rec->behavior);
    fputs(sb_finish(&sb), stdout);
}

/* Intent and behavior from -i/-b, or from the sections of a -F file. */
static bool message_args(Arena *a, int32_t argc, char **argv,
                         const char *const *value_flags, bool json,
                         const char **intent, const char **behavior) {
    const char *i = flag_value2(argc, argv, value_flags, "-i", "--intent");
    const char *b = flag_value2(argc, argv, value_flags, "-b", "--behavior");
    const char *f = flag_value(argc, argv, value_flags, "-F");
    char err[512];
    if (f) {
        if (i || b) {
            err_out(json, "usage",
                    "give the message with -i and -b, or with -F, not both");
            return false;
        }
        char *text;
        if (!read_text_arg(a, f, &text, err, sizeof err) ||
            !msg_parse_file(a, text, intent, behavior, err, sizeof err)) {
            err_out(json, "bad_message_file", "%s", err);
            return false;
        }
        return true;
    }
    char *ti = i ? arena_strdup(a, i) : NULL;
    char *tb = b ? arena_strdup(a, b) : NULL;
    if (ti)
        msg_trim(ti);
    if (tb)
        msg_trim(tb);
    if (!ti || !ti[0]) {
        err_out(json, "missing_intent",
                "a commit needs an intent: -i \"why this edit exists\" (or "
                "-F <file> with Intent: and Behavior: sections)");
        return false;
    }
    if (!tb || !tb[0]) {
        err_out(json, "missing_behavior",
                "a commit needs a behavior: -b \"what this edit makes the "
                "code do\"");
        return false;
    }
    *intent = ti;
    *behavior = tb;
    return true;
}

int32_t cmd_commit(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {
        "-i", "--intent", "-b", "--behavior", "-F", "--edit", "--lines",
        NULL};
    static const char *const bool_flags[] = {"--json", "--no-session",
                                             "--force-message", "--dry-run",
                                             NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    bool force = has_flag(argc, argv, value_flags, "--force-message");
    /* A dry run opens the repository as a reader: no lock, no torn-tail
     * repair, counters healed in memory only. Everything up to the append
     * is the same code, so its errors are the commit's errors. */
    bool dry = has_flag(argc, argv, value_flags, "--dry-run");
    const char *file_arg = positional_arg(argc, argv, value_flags, 0);
    const char *edit_arg = flag_value(argc, argv, value_flags, "--edit");
    const char *lines_arg = flag_value(argc, argv, value_flags, "--lines");
    bool no_session = has_flag(argc, argv, value_flags, "--no-session");

    if (!file_arg) {
        err_out(json, "usage",
                "usage: lap commit <file> (-i \"intent\" -b \"behavior\" | "
                "-F <file|->) [--edit <n> | --lines <a>-<b>] "
                "[--force-message] [--no-session] [--dry-run] [--json]");
        return LAP_EXIT_ERR;
    }
    const char *intent = NULL, *behavior = NULL;
    if (!message_args(a, argc, argv, value_flags, json, &intent, &behavior))
        return LAP_EXIT_ERR;
    if (edit_arg && lines_arg) {
        err_out(json, "usage", "--edit and --lines are mutually exclusive");
        return LAP_EXIT_ERR;
    }

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, !dry, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }

    int32_t rc = LAP_EXIT_ERR;
    char rel[LAP_PATH_MAX];
    if (!repo_relpath(&repo, file_arg, rel, sizeof rel, err, sizeof err)) {
        err_out(json, "bad_path", "%s", err);
        goto done;
    }

    const Ignore *ig = ignore_load(a, repo.root);
    if (ignore_match(ig, rel, false)) {
        err_out(json, "ignored", "%s is ignored (see %s)", rel,
                LAP_IGNORE_NAME);
        goto done;
    }

    if (!repo.active_session[0] && !no_session) {
        err_out(json, "no_session",
                "no active session; start one (lap session start \"...\") or "
                "commit outside sessions explicitly with --no-session");
        goto done;
    }

    FileDiff fd;
    if (!file_diff_load(a, &repo, rel, &fd, err, sizeof err)) {
        err_out(json, "read_failed", "%s", err);
        goto done;
    }
    if (fd.binary) {
        err_out(json, "binary", "%s looks binary; lap tracks text files only",
                rel);
        goto done;
    }
    if (!fd.work_exists && !fd.shadow_exists) {
        err_out(json, "unknown_file", "%s does not exist", rel);
        goto done;
    }

    Rec rec;
    memset(&rec, 0, sizeof rec);
    rec.type = REC_COMMIT;
    rec.file = rel;
    rec.intent = intent;
    rec.behavior = behavior;
    rec.forced = force;
    rec.user = repo_user(&repo);
    rec.session = no_session ? NULL
                  : (repo.active_session[0]
                         ? arena_strdup(a, repo.active_session)
                         : NULL);

    if (fd.work_exists && !fd.shadow_exists) {
        /* whole file is one edit (like git: empty history, one big change) */
        rec.op = "create";
        rec.old_start = 1;
        rec.old_lines = 0;
        rec.new_start = 1;
        rec.new_lines = fd.work.count;
        rec.old_text = NULL;
        rec.old_n = 0;
        rec.new_text = fd.work.lines;
        rec.new_n = fd.work.count;
        rec.eof_nl = fd.work.eof_nl;
    } else if (!fd.work_exists && fd.shadow_exists) {
        rec.op = "delete";
        rec.old_start = 1;
        rec.old_lines = fd.shadow.count;
        rec.new_start = 1;
        rec.new_lines = 0;
        rec.old_text = fd.shadow.lines;
        rec.old_n = fd.shadow.count;
        rec.new_text = NULL;
        rec.new_n = 0;
        rec.eof_nl = true;
    } else {
        if (fd.regions.count == 0) {
            err_out(json, "no_changes", "no changes in %s since last commit",
                    rel);
            goto done;
        }
        Region *chosen = NULL;
        if (fd.regions.count == 1 && !edit_arg && !lines_arg) {
            chosen = &fd.regions.v[0];
        } else if (edit_arg) {
            char *end = NULL;
            long idx = strtol(edit_arg, &end, 10);
            if (end == edit_arg || *end != '\0' || idx < 1 ||
                idx > fd.regions.count) {
                err_out(json, "bad_edit_index",
                        "--edit %s is out of range (1-%d)", edit_arg,
                        fd.regions.count);
                goto done;
            }
            chosen = &fd.regions.v[idx - 1];
        } else if (lines_arg) {
            int32_t la, lb;
            if (!parse_lines_arg(lines_arg, &la, &lb)) {
                err_out(json, "bad_lines",
                        "--lines expects <start>-<end> (1-based, inclusive)");
                goto done;
            }
            for (int32_t i = 0; i < fd.regions.count; i++) {
                if (region_matches_lines(&fd.regions.v[i], la, lb)) {
                    chosen = &fd.regions.v[i];
                    break;
                }
            }
            if (!chosen) {
                err_out(json, "lines_mismatch",
                        "--lines %s does not match a detected edit; run "
                        "\"lap status\" to see the current edit ranges",
                        lines_arg);
                goto done;
            }
        } else {
            print_multi_edit_error(a, json, rel, &fd.regions);
            goto done;
        }

        rec.op = "edit";
        rec.old_start = chosen->old_start;
        rec.old_lines = chosen->old_lines;
        /* The committed file = shadow with ONLY this region applied, so the
         * region's position there is old_start — NOT the working-file
         * new_start, which counts through other still-pending regions
         * above. Blame and hunk headers read new_start as a committed-file
         * coordinate; storing the working-file number mis-attributes lines.
         */
        rec.new_start = chosen->old_start;
        rec.new_lines = chosen->new_lines;
        rec.old_text = fd.shadow.lines + (chosen->old_start - 1);
        rec.old_n = chosen->old_lines;
        rec.new_text = fd.work.lines + (chosen->new_start - 1);
        rec.new_n = chosen->new_lines;
        /* the committed file's trailing-newline state: the working file's if
         * this region reaches the end of both files, else the shadow's */
        bool touches_end =
            chosen->new_start + chosen->new_lines - 1 >= fd.work.count ||
            chosen->old_start + chosen->old_lines - 1 >= fd.shadow.count;
        rec.eof_nl = touches_end ? fd.work.eof_nl : fd.shadow.eof_nl;
    }

    MsgInput check = {intent, behavior, NULL, NULL, 0, force};
    if (rec.session)
        check.prev_behavior = session_last_behavior(a, &repo, rec.session);
    bool pure_delete = rec.new_n == 0;
    check.code = pure_delete ? rec.old_text : rec.new_text;
    check.code_n = pure_delete ? rec.old_n : rec.new_n;
    char why[256];
    const char *refused = msg_check(a, &check, why, sizeof why);
    if (refused) {
        err_out(json, refused, "%s%s", why,
                strcmp(refused, "message_too_short") == 0
                    ? ""
                    : " (--force-message if this is honestly the same)");
        goto done;
    }

    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "L%lld", (long long)repo.next_commit);
    rec.id = idbuf;
    if (dry) {
        print_dry_run(a, json, &rec, rel);
        rc = LAP_EXIT_OK;
        goto done;
    }
    repo.next_commit++;

    if (!repo_append(&repo, &rec, err, sizeof err)) {
        err_out(json, "append_failed", "%s", err);
        rc = LAP_EXIT_FATAL;
        goto done;
    }

    /* Update the shadow, THEN persist state: a crash anywhere in between
     * leaves state.json behind the log tail, which the next writer detects
     * and heals (including a full shadow rebuild). */
    bool shadow_ok = true;
    char *snap_bytes = NULL;
    size_t snap_len = 0;
    if (strcmp(rec.op, "delete") == 0) {
        shadow_ok = shadow_remove(&repo, rel);
    } else {
        Lines base = fd.shadow_exists ? fd.shadow : (Lines){NULL, 0, true};
        Lines after = lines_replace(a, base, rec.old_start, rec.old_lines,
                                    rec.new_text, rec.new_n, rec.eof_nl);
        snap_bytes = join_lines(a, after, &snap_len);
        shadow_ok = shadow_write(&repo, rel, snap_bytes, snap_len);
    }
    if (!shadow_ok) {
        err_out(json, "shadow_failed",
                "commit %s recorded, but updating the shadow copy failed; "
                "the next lap command will heal (or run \"lap verify "
                "--deep\")",
                rec.id);
        rc = LAP_EXIT_FATAL;
        goto done;
    }
    if (!repo_state_save(&repo, err, sizeof err)) {
        err_out(json, "state_failed", "%s", err);
        rc = LAP_EXIT_FATAL;
        goto done;
    }
    caches_sync_warn(a, &repo);
    if (snap_bytes) {
        Idx *idx = idx_ready(a, &repo);
        char serr[256];
        if (idx && !snap_maybe(a, &repo, idx, rel, snap_bytes, snap_len,
                               serr, sizeof serr))
            fprintf(stderr, "lap: snapshot: %s\n", serr);
    }

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb, "{\"ok\":true,\"id\":\"%s\",\"hash\":\"%s\",\"file\":",
                  rec.id, rec.hash);
        json_escape_c(&sb, rel);
        sb_printf(&sb,
                  ",\"op\":\"%s\",\"session\":%s%s%s,\"old_start\":%d,"
                  "\"old_lines\":%d,\"new_start\":%d,\"new_lines\":%d}",
                  rec.op, rec.session ? "\"" : "",
                  rec.session ? rec.session : "null", rec.session ? "\"" : "",
                  rec.old_start, rec.old_lines, rec.new_start, rec.new_lines);
        puts(sb_finish(&sb));
    } else {
        Region shown;
        shown.old_start = rec.old_start;
        shown.old_lines = rec.old_lines;
        shown.new_start = rec.new_start;
        shown.new_lines = rec.new_lines;
        char desc[128];
        region_describe(&shown, desc, sizeof desc);
        const char *nl = strchr(intent, '\n');
        int32_t mlen = nl ? (int32_t)(nl - intent) : (int32_t)strlen(intent);
        char sh[SHORT_HASH_LEN + 1];
        short_hash(&rec, sh);
        printf("[%s%s %s%s] %s%s%s %s: %s%s%s  \"%.*s\"\n", sgr(S_ID), rec.id,
               sh, sgr_off(), sgr(S_SESSION),
               rec.session ? rec.session : "(no session)", sgr_off(), rel,
               sgr(S_MUTED), desc, sgr_off(), mlen, intent);
    }
    rc = LAP_EXIT_OK;

done:
    repo_close(&repo);
    return rc;
}
