#include "cmd.h"

/* lap amend: appends an amend record that gives a commit a new intent and
 * behavior. Nothing already written changes; readers show the latest text
 * (rec_amend_log, idx_fetch). */
int32_t cmd_amend(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {
        "-i", "--intent", "-b", "--behavior", "-F", "--branch", NULL};
    static const char *const bool_flags[] = {"--json", "--force-message",
                                             NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    bool force = has_flag(argc, argv, value_flags, "--force-message");
    const char *ref = positional_arg(argc, argv, value_flags, 0);
    if (!ref) {
        err_out(json, "usage",
                "usage: lap amend <commit> (-i \"intent\" -b \"behavior\" | "
                "-F <file|->) [--force-message] [--branch <name>] [--json]");
        return LAP_EXIT_ERR;
    }
    const char *intent = NULL, *behavior = NULL;
    if (!message_args(a, argc, argv, value_flags, json, &intent, &behavior))
        return LAP_EXIT_ERR;

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
    RecLog log;
    if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        goto done;
    }
    const char *code;
    int32_t at = ref_find_record(&log, ref, &code, err, sizeof err);
    if (at < 0) {
        err_out(json, code, "%s", err);
        goto done;
    }
    Rec *c = &log.v[at];
    if (c->type != REC_COMMIT) {
        err_out(json, "not_a_commit",
                "%s is not a commit: it names a %s record; lap amend "
                "corrects commits",
                ref, rec_type_name(c->type));
        goto done;
    }
    /* a branch's history starts with its parent's, which only the parent
     * can correct */
    if (at < own_part_start(&log, repo.hist.parent[0] ? repo.hist.lineage
                                                       : NULL)) {
        err_out(json, "not_own_commit",
                "%s is from before this branch started; amend it where it "
                "was made",
                c->id);
        goto done;
    }
    if (strcmp(c->intent, intent) == 0 &&
        strcmp(c->behavior, behavior) == 0) {
        err_out(json, "same_message",
                "%s already says that; nothing to amend", c->id);
        goto done;
    }

    /* the checks a commit's message passes, against this commit's session
     * and edit */
    MsgInput check = {intent, behavior, NULL, NULL, 0, force};
    for (int32_t i = at - 1; c->session && i >= 0; i--) {
        const Rec *p = &log.v[i];
        if (p->type == REC_SESSION_START && p->id &&
            strcmp(p->id, c->session) == 0)
            break;
        if (p->type == REC_COMMIT && p->session &&
            strcmp(p->session, c->session) == 0) {
            check.prev_behavior = p->behavior;
            break;
        }
    }
    bool pure_delete = c->new_n == 0;
    check.code = pure_delete ? c->old_text : c->new_text;
    check.code_n = pure_delete ? c->old_n : c->new_n;
    char why[256];
    const char *refused = msg_check(a, &check, why, sizeof why);
    if (refused) {
        err_out(json, refused, "%s%s", why,
                strcmp(refused, "message_too_short") == 0
                    ? ""
                    : " (--force-message if this is honestly the same)");
        goto done;
    }

    Rec am;
    memset(&am, 0, sizeof am);
    am.type = REC_AMEND;
    am.of = c->hash;
    am.user = repo_user(&repo);
    am.intent = intent;
    am.behavior = behavior;
    am.forced = force;
    if (!repo_append(&repo, &am, err, sizeof err)) {
        err_out(json, "append_failed", "%s", err);
        rc = LAP_EXIT_FATAL;
        goto done;
    }
    if (!repo_state_save(&repo, err, sizeof err)) {
        err_out(json, "state_failed", "%s", err);
        rc = LAP_EXIT_FATAL;
        goto done;
    }
    caches_sync_warn(a, &repo);
    int32_t count = c->amended + 1; /* amendments, this one included */

    char sh[SHORT_HASH_LEN + 1];
    short_hash(c, sh);
    if (json) {
        printf("{\"ok\":true,\"id\":\"%s\",\"hash\":\"%s\",\"amended\":%d}\n",
               c->id, c->hash, count);
    } else {
        const char *nl = strchr(intent, '\n');
        int32_t mlen = nl ? (int32_t)(nl - intent) : (int32_t)strlen(intent);
        printf("[%s%s %s%s] amended (%d)  \"%.*s\"\n", sgr(S_ID), c->id, sh,
               sgr_off(), count, mlen, intent);
    }
    rc = LAP_EXIT_OK;

done:
    repo_close(&repo);
    return rc;
}
