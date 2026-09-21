#include "cmd.h"

typedef struct {
    uint32_t session; /* 0 = no session filter */
    int32_t file_id;  /* -1 = no file filter (index path only) */
    const char *rel;  /* "" = no file filter */
    int64_t limit;    /* <0 = unlimited */
} LogQuery;

static void emit(StrBuf *sb, const Rec *rec, bool json, int64_t printed) {
    if (json) {
        if (printed)
            sb_putc(sb, ',');
        sb_putc(sb, '{');
        json_commit(sb, rec, NULL);
        sb_putc(sb, '}');
    } else {
        print_commit_human(sb, rec, true, NULL);
    }
}

/* Index path: entries carry session and file, so unmatched commits never
 * reach the log. Returns false if the index turns out to be unusable, and
 * the caller redoes the query by scanning — a broken cache costs speed,
 * never answers. */
static bool log_via_index(Arena *a, Repo *repo, const Idx *ix,
                          const LogQuery *q, StrBuf *sb, bool json,
                          int64_t *printed) {
    for (int64_t e = (int64_t)ix->h.count - 1; e >= 0; e--) {
        const IdxEntry *en = &ix->v[e];
        if (en->kind != IDX_COMMIT ||
            (q->session && en->session != q->session) ||
            (q->file_id >= 0 && en->file_id != (uint32_t)q->file_id))
            continue;
        if (q->limit >= 0 && *printed >= q->limit)
            break;
        Rec rec;
        if (!idx_fetch(a, repo, ix, e, &rec))
            return false;
        emit(sb, &rec, json, (*printed)++);
    }
    return true;
}

static bool log_via_scan(Arena *a, Repo *repo, const LogQuery *q, StrBuf *sb,
                         bool json, int64_t *printed, char *err,
                         size_t errsz) {
    RecLog log;
    if (!rec_log_load(a, repo->logpath, &log, err, errsz))
        return false;
    for (int32_t i = log.count - 1; i >= 0; i--) {
        const Rec *rec = &log.v[i];
        if (rec->type != REC_COMMIT ||
            (q->session && rec_session_no(rec->session) != q->session) ||
            (q->rel[0] && strcmp(rec->file, q->rel) != 0))
            continue;
        if (q->limit >= 0 && *printed >= q->limit)
            break;
        emit(sb, rec, json, (*printed)++);
    }
    return true;
}

int32_t cmd_log(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {"--session", "--file", "-n",
                                              NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    const char *filt_session = flag_value(argc, argv, value_flags,
                                          "--session");
    const char *filt_file = flag_value(argc, argv, value_flags, "--file");
    const char *limit_arg = flag_value(argc, argv, value_flags, "-n");

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }

    LogQuery q;
    q.session = rec_session_no(filt_session);
    q.file_id = -1;
    q.limit = limit_arg ? strtoll(limit_arg, NULL, 10) : -1;
    char rel[LAP_PATH_MAX] = "";
    if (filt_file &&
        !repo_relpath(&repo, filt_file, rel, sizeof rel, err, sizeof err)) {
        err_out(json, "bad_path", "%s", err);
        return LAP_EXIT_ERR;
    }
    q.rel = rel;
    /* a session filter that is not a session id matches nothing, on both
     * paths alike */
    bool none = filt_session && !q.session;

    StrBuf sb;
    int64_t printed = 0;
    bool served = false;
    Idx *ix = none ? NULL : idx_ready(a, &repo);
    if (ix) {
        q.file_id = rel[0] ? idx_file_id(ix, rel) : -1;
        if (!(rel[0] && q.file_id < 0)) { /* unknown file: nothing to list */
            sb_init(&sb, a);
            if (json)
                sb_puts(&sb, "{\"ok\":true,\"commits\":[");
            served = log_via_index(a, &repo, ix, &q, &sb, json, &printed);
        } else {
            served = true;
            sb_init(&sb, a);
            if (json)
                sb_puts(&sb, "{\"ok\":true,\"commits\":[");
        }
    }
    if (!served) {
        printed = 0;
        sb_init(&sb, a); /* discard any partial index output */
        if (json)
            sb_puts(&sb, "{\"ok\":true,\"commits\":[");
        if (!none &&
            !log_via_scan(a, &repo, &q, &sb, json, &printed, err,
                          sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            return LAP_EXIT_ERR;
        }
    }

    if (json) {
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        if (!printed)
            sb_puts(&sb, "no commits yet\n");
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}
