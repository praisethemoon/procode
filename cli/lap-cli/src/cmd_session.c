#include "cmd.h"

static const char *first_line(Arena *a, const char *s) {
    const char *nl = strchr(s, '\n');
    if (!nl)
        return s;
    return arena_strndup(a, s, (size_t)(nl - s));
}

/* Every `--meta key=value` on the command line, in order. A repeated key is
 * refused rather than last-wins: two tickets on one session is a question,
 * not an answer. */
static bool meta_args(Arena *a, int32_t argc, char **argv, Rec *rec,
                      char *err, size_t errsz) {
    int32_t cap = 0;
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            break;
        if (strcmp(argv[i], "--meta") == 0)
            cap++;
    }
    if (cap == 0)
        return true;
    rec->meta_keys = (const char **)arena_alloc(a, (size_t)cap * sizeof(char *));
    rec->meta_vals = (const char **)arena_alloc(a, (size_t)cap * sizeof(char *));
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            break;
        if (strcmp(argv[i], "--meta") != 0)
            continue;
        const char *kv = i + 1 < argc ? argv[++i] : "";
        const char *key, *val;
        if (!rec_meta_parse(a, kv, &key, &val, err, errsz))
            return false;
        for (int32_t k = 0; k < rec->meta_n; k++) {
            if (strcmp(rec->meta_keys[k], key) == 0) {
                snprintf(err, errsz, "meta key \"%s\" given twice", key);
                return false;
            }
        }
        rec->meta_keys[rec->meta_n] = key;
        rec->meta_vals[rec->meta_n] = val;
        rec->meta_n++;
    }
    return true;
}

/* A session matches a filter when it carries every key with the same value. */
static bool meta_match(const Rec *st, const Rec *want) {
    for (int32_t k = 0; k < want->meta_n; k++) {
        const char *v = rec_meta(st, want->meta_keys[k]);
        if (!v || strcmp(v, want->meta_vals[k]) != 0)
            return false;
    }
    return true;
}

static int32_t session_list(Arena *a, Repo *repo, bool json,
                            const Rec *filter) {
    RecLog log;
    char err[512];
    if (!repo_log_load(a, repo, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }
    StrBuf sb;
    sb_init(&sb, a);
    char err_scratch[128];
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"sessions\":[");
    int32_t printed = 0;
    for (int32_t i = 0; i < log.count; i++) {
        if (log.v[i].type != REC_SESSION_START)
            continue;
        const Rec *st = &log.v[i];
        if (!meta_match(st, filter))
            continue;
        const char *end_ts = NULL;
        int32_t commits = 0;
        for (int32_t j = i + 1; j < log.count; j++) {
            if (log.v[j].type == REC_SESSION_END &&
                strcmp(log.v[j].id, st->id) == 0) {
                end_ts = log.v[j].ts;
                break;
            }
            if (log.v[j].type == REC_COMMIT && log.v[j].session &&
                strcmp(log.v[j].session, st->id) == 0)
                commits++;
        }
        /* another branch's history (--branch): its open session is its
         * active one, whatever this folder has open */
        bool active = repo->foreign
                          ? !end_ts && !st->from
                          : !end_ts && repo->active_session[0] &&
                                strcmp(repo->active_session, st->id) == 0;
        if (json) {
            if (printed)
                sb_putc(&sb, ',');
            sb_printf(&sb, "{\"id\":\"%s\",\"ref\":\"%s\",\"hash\":\"%s\"",
                      st->id, session_ref(a, st->lineage, st->id), st->hash);
            if (st->from)
                sb_printf(&sb, ",\"from\":\"%s\"", st->from);
            sb_puts(&sb, ",\"msg\":");
            json_escape_c(&sb, st->msg);
            sb_printf(&sb, ",\"started\":\"%s\"", st->ts);
            if (end_ts)
                sb_printf(&sb, ",\"ended\":\"%s\"", end_ts);
            else
                sb_puts(&sb, ",\"ended\":null");
            sb_printf(&sb, ",\"commits\":%d,\"active\":%s", commits,
                      active ? "true" : "false");
            rec_meta_json(&sb, st);
            sb_putc(&sb, '}');
        } else {
            sb_field(&sb, active ? S_ACTIVE : S_SESSION,
                     session_ref(a, st->lineage, st->id), 6);
            sb_putc(&sb, ' ');
            char when[40];
            plat_ts_local(st->ts, false, when);
            sb_field(&sb, S_MUTED, when, 0);
            sb_printf(&sb, "  %2d commit%s  ", commits,
                      commits == 1 ? " " : "s");
            const char *fl = first_line(a, st->msg);
            sb_text(&sb, fl, strlen(fl));
            for (int32_t k = 0; k < st->meta_n; k++) {
                /* Strings print without their quotes: ticket=T-12. */
                const char *mv = st->meta_vals[k];
                size_t ml = strlen(mv);
                if (ml >= 2 && mv[0] == '"') {
                    JVal *jv = json_parse(a, mv, ml, err_scratch,
                                          sizeof err_scratch);
                    if (jv && jv->t == J_STR) {
                        mv = jv->s.ptr;
                        ml = jv->s.len;
                    }
                }
                sb_puts(&sb, "  ");
                sb_field(&sb, S_MUTED, st->meta_keys[k], 0);
                sb_putc(&sb, '=');
                sb_text(&sb, mv, ml);
            }
            if (active || !end_ts) {
                sb_puts(&sb, "  ");
                sb_field(&sb, active ? S_ACTIVE : S_MUTED,
                         active ? "(active)" : "(open)", 0);
            }
            sb_putc(&sb, '\n');
        }
        printed++;
    }
    if (json) {
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        if (!printed)
            sb_puts(&sb, "no sessions yet\n");
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}

int32_t cmd_session(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {"-F", "--meta", "--branch",
                                              NULL};
    static const char *const bool_flags[] = {"--json", NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    const char *sub = positional_arg(argc, argv, value_flags, 0);

    Repo repo;
    char err[512];
    bool writing = sub && (strcmp(sub, "start") == 0 ||
                           strcmp(sub, "end") == 0);
    if (!repo_open(a, &repo, writing, err, sizeof err)) {
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;
    Rec meta;
    memset(&meta, 0, sizeof meta);
    if (!meta_args(a, argc, argv, &meta, err, sizeof err)) {
        err_out(json, "bad_meta", "%s", err);
        goto done;
    }

    if (!sub || strcmp(sub, "current") == 0) {
        const char *ref = session_ref(
            a, repo.hist.parent[0] ? repo.hist.name : LAP_MAIN_LINEAGE,
            repo.active_session);
        if (json) {
            StrBuf sb;
            sb_init(&sb, a);
            if (repo.active_session[0]) {
                sb_printf(&sb, "{\"ok\":true,\"session\":{\"id\":\"%s\","
                               "\"ref\":\"%s\",\"msg\":",
                          repo.active_session, ref);
                json_escape_c(&sb, repo.active_session_msg);
                /* The metadata lives on the session_start record, not in
                 * state.json, so it is read back from the log. */
                RecLog log;
                Rec none;
                memset(&none, 0, sizeof none);
                const Rec *st = &none;
                if (repo_log_load(a, &repo, &log, err, sizeof err)) {
                    for (int32_t i = log.count - 1; i >= 0; i--) {
                        if (log.v[i].type == REC_SESSION_START &&
                            strcmp(log.v[i].id, repo.active_session) == 0) {
                            st = &log.v[i];
                            break;
                        }
                    }
                }
                rec_meta_json(&sb, st);
                sb_puts(&sb, "}}");
            } else {
                sb_puts(&sb, "{\"ok\":true,\"session\":null}");
            }
            puts(sb_finish(&sb));
        } else {
            if (repo.active_session[0])
                printf("%s%s%s \"%s\"\n", sgr(S_ACTIVE), ref, sgr_off(),
                       first_line(a, repo.active_session_msg));
            else
                printf("no active session\n");
        }
        rc = LAP_EXIT_OK;
    } else if (strcmp(sub, "list") == 0) {
        /* on list, --branch reads that branch's sessions, as on log */
        if (!repo_view_branch(a, &repo,
                              flag_value(argc, argv, value_flags, "--branch"),
                              json))
            goto done;
        rc = session_list(a, &repo, json, &meta);
    } else if (strcmp(sub, "start") == 0) {
        if (!branch_check(a, &repo, branch_given(argc, argv, value_flags),
                          json))
            goto done;
        const char *msg = positional_arg(argc, argv, value_flags, 1);
        const char *file = flag_value(argc, argv, value_flags, "-F");
        if (msg && file) {
            err_out(json, "usage",
                    "give the purpose as an argument or with -F, not both");
            goto done;
        }
        if (file) {
            char *text;
            if (!read_text_arg(a, file, &text, err, sizeof err)) {
                err_out(json, "bad_message_file", "%s", err);
                goto done;
            }
            msg = text;
        }
        if (!msg || msg[0] == '\0') {
            err_out(json, "message_required",
                    "a session needs a purpose: lap session start \"fix the "
                    "parser bug\" (or -F <file>, -F - for stdin)");
            goto done;
        }
        if (repo.active_session[0]) {
            err_out(json, "session_active",
                    "session %s is already active; end it first (lap session "
                    "end)",
                    repo.active_session);
            goto done;
        }
        Rec rec;
        memset(&rec, 0, sizeof rec);
        rec.type = REC_SESSION_START;
        rec.user = repo_user(&repo);
        char idbuf[32];
        snprintf(idbuf, sizeof idbuf, "S%lld",
                 (long long)repo.next_session);
        rec.id = idbuf;
        rec.msg = msg;
        rec.meta_keys = meta.meta_keys;
        rec.meta_vals = meta.meta_vals;
        rec.meta_n = meta.meta_n;
        repo.next_session++;
        snprintf(repo.active_session, sizeof repo.active_session, "%s",
                 idbuf);
        snprintf(repo.active_session_msg, sizeof repo.active_session_msg,
                 "%s", msg);
        if (!repo_append(&repo, &rec, err, sizeof err) ||
            !repo_state_save(&repo, err, sizeof err)) {
            err_out(json, "append_failed", "%s", err);
            rc = LAP_EXIT_FATAL;
            goto done;
        }
        caches_sync_warn(a, &repo);
        /* in a branch folder the session is named with its branch: that is
         * what an agent copies into a ticket comment */
        const char *ref = session_ref(
            a, repo.hist.parent[0] ? repo.hist.name : LAP_MAIN_LINEAGE, idbuf);
        if (json)
            printf("{\"ok\":true,\"id\":\"%s\",\"ref\":\"%s\"}\n", idbuf, ref);
        else
            printf("session %s%s%s started: %s\n", sgr(S_ACTIVE), ref,
                   sgr_off(), first_line(a, msg));
        rc = LAP_EXIT_OK;
    } else if (strcmp(sub, "end") == 0) {
        if (!repo.active_session[0]) {
            err_out(json, "no_active_session", "no active session to end");
            goto done;
        }
        Rec rec;
        memset(&rec, 0, sizeof rec);
        rec.type = REC_SESSION_END;
        rec.id = arena_strdup(a, repo.active_session);
        repo.active_session[0] = '\0';
        repo.active_session_msg[0] = '\0';
        if (!repo_append(&repo, &rec, err, sizeof err) ||
            !repo_state_save(&repo, err, sizeof err)) {
            err_out(json, "append_failed", "%s", err);
            rc = LAP_EXIT_FATAL;
            goto done;
        }
        caches_sync_warn(a, &repo);
        /* named with its branch, as session start names it */
        const char *ref = session_ref(
            a, repo.hist.parent[0] ? repo.hist.name : LAP_MAIN_LINEAGE,
            rec.id);
        if (json)
            printf("{\"ok\":true,\"id\":\"%s\",\"ref\":\"%s\"}\n", rec.id,
                   ref);
        else
            printf("session %s%s%s ended\n", sgr(S_SESSION), ref, sgr_off());
        rc = LAP_EXIT_OK;
    } else {
        err_out(json, "usage",
                "usage: lap session [start \"purpose\" [--meta k=v]... | end | "
                "list [--meta k=v]... | current]");
    }

done:
    repo_close(&repo);
    return rc;
}
