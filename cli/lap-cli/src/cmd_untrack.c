/* lap untrack: stop tracking files lap recorded, keeping their history and
 * the files themselves (SPEC.md, lap untrack).
 *
 * The way out for a file recorded before .lapignore named it, short of a
 * new history. Each file gets one commit whose op is "untrack": in the
 * session, with an intent, like any commit, and no text. Replaying it takes
 * the file out of the tracked set, as a delete does, but lap never touches
 * the working file, so replaying it elsewhere (another clone, a branch's
 * merge, a rebuild) removes nothing from anyone's disk.
 *
 * All or nothing: every path is checked before anything is written. A path
 * lap does not track is refused, and so is a file with edits never
 * committed, whose changes would leave lap unseen; --force untracks it
 * anyway (the edits stay on disk). */

#include "cmd.h"
#include "help.h"

/* What the record says it does: the same for every untracked file. */
#define UNTRACK_BEHAVIOR                                                     \
    "lap stops tracking this file; it stays on disk and its history stays " \
    "readable"

/* The tracked files rel names: rel itself, or with rel a folder (or the
 * repository's root, ""), every tracked file under it. */
static size_t matches(const char *rel, const char *const *tracked,
                      size_t ntracked, const char **out) {
    size_t n = 0, len = strlen(rel);
    for (size_t i = 0; i < ntracked; i++) {
        const char *t = tracked[i];
        if (strcmp(t, rel) == 0 ||
            len == 0 || (strncmp(t, rel, len) == 0 && t[len] == '/'))
            out[n++] = t;
    }
    return n;
}

/* Whether a file has edits lap has not recorded: changed, deleted, or
 * unreadable (which hides whether it changed). */
static bool has_pending(Arena *a, Repo *r, const char *rel) {
    FileDiff fd;
    char err[256];
    if (!file_diff_load(a, r, rel, &fd, err, sizeof err))
        return fd.unreadable;
    if (fd.binary)
        return false; /* no edits lap could record anyway */
    return !fd.work_exists || fd.regions.count > 0;
}

int32_t cmd_untrack(Arena *a, int32_t argc, char **argv) {
    FlagSets fs;
    help_flag_sets("untrack", &fs);
    const char *const *value_flags = fs.values;
    const char *const *bool_flags = fs.bools;
    bool json = has_flag(argc, argv, value_flags, "--json");
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    bool force = has_flag(argc, argv, value_flags, "--force");
    bool no_session = has_flag(argc, argv, value_flags, "--no-session");
    const char *intent =
        flag_value2(argc, argv, value_flags, "-i", "--intent");
    if (!positional_arg(argc, argv, value_flags, 0) || !intent) {
        err_out(json, "usage", "usage: lap %s", help_synopsis("untrack"));
        return LAP_EXIT_ERR;
    }
    char *trimmed = arena_strdup(a, intent);
    msg_trim(trimmed);
    intent = trimmed;
    if (!intent[0]) {
        err_out(json, "usage", "the intent is empty: say why lap stops "
                               "tracking these files");
        return LAP_EXIT_ERR;
    }

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, true, err, sizeof err)) {
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    if (!branch_check(a, &repo, branch_given(argc, argv, value_flags),
                      json)) {
        repo_close(&repo);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;
    if (!repo.active_session[0] && !no_session) {
        err_out(json, "no_session",
                "no active session; start one (lap session start \"...\") or "
                "untrack outside sessions explicitly with --no-session");
        goto done;
    }

    const char **tracked;
    size_t ntracked;
    if (!tracked_files(a, &repo, &tracked, &ntracked, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }

    /* Every path named, each file once, in the order they were named. */
    const char **files =
        (const char **)arena_alloc(a, (ntracked ? ntracked : 1) * sizeof(char *));
    size_t nfiles = 0;
    StrMap seen;
    memset(&seen, 0, sizeof seen);
    const char **hit =
        (const char **)arena_alloc(a, (ntracked ? ntracked : 1) * sizeof(char *));
    for (int32_t k = 0;; k++) {
        const char *arg = positional_arg(argc, argv, value_flags, k);
        if (!arg)
            break;
        char rel[LAP_PATH_MAX];
        if (!repo_relpath(&repo, arg, rel, sizeof rel, err, sizeof err)) {
            err_out(json, "bad_path", "%s", err);
            goto done;
        }
        size_t nh = matches(rel, tracked, ntracked, hit);
        if (nh == 0) {
            err_out(json, "not_tracked", "%s is not tracked by lap",
                    rel[0] ? rel : ".");
            goto done;
        }
        for (size_t i = 0; i < nh; i++) {
            if (strmap_get(&seen, hit[i]))
                continue;
            strmap_put(a, &seen, hit[i], "");
            files[nfiles++] = hit[i];
        }
    }

    /* Refused whole when one file has edits lap never recorded. */
    if (!force) {
        Arena *fa = arena_new(1 << 16);
        const char *first = NULL;
        size_t npending = 0;
        for (size_t i = 0; i < nfiles; i++) {
            arena_reset(fa);
            if (has_pending(fa, &repo, files[i])) {
                if (!first)
                    first = files[i];
                npending++;
            }
        }
        arena_free(fa);
        if (npending) {
            err_out(json, "pending_edits",
                    "%s%s has edits lap has not recorded; commit them first, "
                    "or pass --force to untrack anyway (the edits stay on "
                    "disk, outside lap)",
                    first,
                    npending > 1 ? arena_printf(a, " (and %zu more)",
                                                npending - 1)
                                 : "");
            goto done;
        }
    }

    const Ignore *ig = ignore_load(a, repo.root);
    const char *session =
        no_session ? NULL : arena_strdup(a, repo.active_session);
    const char *lineage =
        repo.hist.parent[0] ? repo.hist.name : LAP_MAIN_LINEAGE;
    Rec *recs = (Rec *)arena_alloc0(a, (nfiles ? nfiles : 1) * sizeof(Rec));
    for (size_t i = 0; i < nfiles; i++) {
        Rec *rec = &recs[i];
        rec->type = REC_COMMIT;
        rec->id = arena_printf(a, "L%lld", (long long)repo.next_commit);
        rec->file = files[i];
        rec->op = "untrack";
        rec->intent = intent;
        rec->behavior = UNTRACK_BEHAVIOR;
        rec->user = repo_user(&repo);
        rec->session = session;
        rec->lineage = lineage;
        rec->eof_nl = true;
        repo.next_commit++;
        if (!repo_append(&repo, rec, err, sizeof err)) {
            err_out(json, "append_failed", "%s", err);
            rc = LAP_EXIT_FATAL;
            goto done;
        }
        /* log, then shadow, then state: a crash between them leaves the
         * state behind the log, which the next writer heals */
        if (!shadow_remove(&repo, files[i])) {
            err_out(json, "shadow_failed",
                    "%s recorded, but removing its shadow copy failed; the "
                    "next lap command will heal (or run \"lap verify "
                    "--deep\")",
                    rec->id);
            rc = LAP_EXIT_FATAL;
            goto done;
        }
    }
    if (!repo_state_save(&repo, err, sizeof err)) {
        err_out(json, "state_failed", "%s", err);
        rc = LAP_EXIT_FATAL;
        goto done;
    }
    caches_sync_warn(a, &repo);

    /* Not ignored and still there: the next status shows it as new. */
    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"session\":");
        if (session)
            json_escape_c(&sb, session);
        else
            sb_puts(&sb, "null");
        sb_puts(&sb, ",\"untracked\":[");
        for (size_t i = 0; i < nfiles; i++) {
            if (i)
                sb_putc(&sb, ',');
            sb_printf(&sb, "{\"id\":\"%s\",\"hash\":\"%s\",\"file\":",
                      recs[i].id, recs[i].hash);
            json_escape_c(&sb, files[i]);
            sb_putc(&sb, '}');
        }
        sb_puts(&sb, "],\"not_ignored\":[");
        bool any = false;
        for (size_t i = 0; i < nfiles; i++) {
            if (ignore_match(ig, files[i], false))
                continue;
            if (any)
                sb_putc(&sb, ',');
            json_escape_c(&sb, files[i]);
            any = true;
        }
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        const char *nl = strchr(intent, '\n');
        int32_t mlen = nl ? (int32_t)(nl - intent) : (int32_t)strlen(intent);
        size_t loose = 0;
        for (size_t i = 0; i < nfiles; i++) {
            char sh[SHORT_HASH_LEN + 1];
            short_hash(&recs[i], sh);
            printf("[%s%s %s%s] %s%s%s %s: %suntracked%s  \"%.*s\"\n",
                   sgr(S_ID), recs[i].id, sh, sgr_off(), sgr(S_SESSION),
                   session ? session_ref(a, lineage, session) : "(no session)",
                   sgr_off(), files[i], sgr(S_MUTED), sgr_off(), mlen,
                   intent);
            if (!ignore_match(ig, files[i], false))
                loose++;
        }
        if (loose) {
            fprintf(stderr,
                    "note: %zu of these %s not in %s, so lap status will "
                    "show %s as new; add %s to %s to keep %s out\n",
                    loose, loose == 1 ? "is" : "are", LAP_IGNORE_NAME,
                    loose == 1 ? "it" : "them", loose == 1 ? "it" : "them",
                    LAP_IGNORE_NAME, loose == 1 ? "it" : "them");
        }
    }
    rc = LAP_EXIT_OK;

done:
    repo_close(&repo);
    return rc;
}
