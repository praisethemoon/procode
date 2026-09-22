#include "cmd.h"

static const char *first_line(Arena *a, const char *s) {
    const char *nl = strchr(s, '\n');
    if (!nl)
        return s;
    return arena_strndup(a, s, (size_t)(nl - s));
}

static int32_t session_list(Arena *a, Repo *repo, bool json) {
    RecLog log;
    char err[512];
    if (!rec_log_load(a, repo->logpath, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }
    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"sessions\":[");
    int32_t printed = 0;
    for (int32_t i = 0; i < log.count; i++) {
        if (log.v[i].type != REC_SESSION_START)
            continue;
        const Rec *st = &log.v[i];
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
        bool active = !end_ts && repo->active_session[0] &&
                      strcmp(repo->active_session, st->id) == 0;
        if (json) {
            if (printed)
                sb_putc(&sb, ',');
            sb_printf(&sb, "{\"id\":\"%s\",\"msg\":", st->id);
            json_escape_c(&sb, st->msg);
            sb_printf(&sb, ",\"started\":\"%s\"", st->ts);
            if (end_ts)
                sb_printf(&sb, ",\"ended\":\"%s\"", end_ts);
            else
                sb_puts(&sb, ",\"ended\":null");
            sb_printf(&sb, ",\"commits\":%d,\"active\":%s}", commits,
                      active ? "true" : "false");
        } else {
            sb_field(&sb, active ? S_ACTIVE : S_SESSION, st->id, 6);
            sb_putc(&sb, ' ');
            sb_field(&sb, S_MUTED, st->ts, 0);
            sb_printf(&sb, "  %2d commit%s  %s", commits,
                      commits == 1 ? " " : "s", first_line(a, st->msg));
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
    static const char *const value_flags[] = {"-m", "-F", NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    const char *sub = positional_arg(argc, argv, value_flags, 0);

    Repo repo;
    char err[512];
    bool writing = sub && (strcmp(sub, "start") == 0 ||
                           strcmp(sub, "end") == 0);
    if (!repo_open(a, &repo, writing, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;

    if (!sub || strcmp(sub, "current") == 0) {
        if (json) {
            StrBuf sb;
            sb_init(&sb, a);
            if (repo.active_session[0]) {
                sb_printf(&sb, "{\"ok\":true,\"session\":{\"id\":\"%s\","
                               "\"msg\":",
                          repo.active_session);
                json_escape_c(&sb, repo.active_session_msg);
                sb_puts(&sb, "}}");
            } else {
                sb_puts(&sb, "{\"ok\":true,\"session\":null}");
            }
            puts(sb_finish(&sb));
        } else {
            if (repo.active_session[0])
                printf("%s%s%s \"%s\"\n", sgr(S_ACTIVE), repo.active_session,
                       sgr_off(), first_line(a, repo.active_session_msg));
            else
                printf("no active session\n");
        }
        rc = LAP_EXIT_OK;
    } else if (strcmp(sub, "list") == 0) {
        rc = session_list(a, &repo, json);
    } else if (strcmp(sub, "start") == 0) {
        const char *msg = NULL;
        char merr[512];
        int32_t mrc = message_arg(a, argc, argv, value_flags, &msg, merr,
                                  sizeof merr);
        if (mrc < 0) {
            err_out(json, "bad_message", "%s", merr);
            goto done;
        }
        if (mrc == 0)
            msg = positional_arg(argc, argv, value_flags, 1);
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
        if (json)
            printf("{\"ok\":true,\"id\":\"%s\"}\n", idbuf);
        else
            printf("session %s%s%s started: %s\n", sgr(S_ACTIVE), idbuf,
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
        if (json)
            printf("{\"ok\":true,\"id\":\"%s\"}\n", rec.id);
        else
            printf("session %s%s%s ended\n", sgr(S_SESSION), rec.id,
                   sgr_off());
        rc = LAP_EXIT_OK;
    } else {
        err_out(json, "usage",
                "usage: lap session [start \"purpose\" | end | list | "
                "current]");
    }

done:
    repo_close(&repo);
    return rc;
}
