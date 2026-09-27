#include "cmd.h"

/* lap verify: checks the log hash chain (and with --deep, replays every
 * file's history and compares the result byte-for-byte with the shadow
 * store).
 */

typedef struct {
    Arena *a;
    const RecLog *log;
    Repo *repo;
    int32_t checked;
    int32_t mismatched;
    StrBuf *out;
    bool json;
} DeepCheck;

static WalkAction on_shadow_file(const char *rel, bool is_dir, void *ud) {
    DeepCheck *dc = (DeepCheck *)ud;
    if (is_dir || plat_is_tmp_name(rel))
        return WALK_CONT;
    dc->checked++;
    Lines replayed;
    bool deleted;
    bool has_history =
        rec_replay_file(dc->a, dc->log, rel, dc->log->count - 1, &replayed,
                        &deleted);
    char spath[LAP_PATH_MAX];
    snprintf(spath, sizeof spath, "%s/%s/%s", dc->repo->lapdir,
             LAP_SHADOW_NAME, rel);
    char *sdata;
    size_t slen;
    bool ok = false;
    if (has_history && !deleted && plat_read_file(dc->a, spath, &sdata,
                                                  &slen)) {
        size_t rlen;
        char *rdata = join_lines(dc->a, replayed, &rlen);
        ok = rlen == slen && memcmp(rdata, sdata, rlen) == 0;
    }
    if (!ok) {
        dc->mismatched++;
        if (dc->json) {
            if (dc->mismatched > 1)
                sb_putc(dc->out, ',');
            json_escape_c(dc->out, rel);
        } else {
            sb_printf(dc->out, "  shadow/log mismatch: %s\n", rel);
        }
    }
    return WALK_CONT;
}

typedef struct {
    Arena *a;
    const RecLog *log;
    DeepCheck *dc;
    const char *snapdir;
} SnapCheck;

static void snap_flag_mismatch(SnapCheck *sc, const char *what) {
    sc->dc->mismatched++;
    if (sc->dc->json) {
        if (sc->dc->mismatched > 1)
            sb_putc(sc->dc->out, ',');
        json_escape_c(sc->dc->out, what);
    } else {
        sb_printf(sc->dc->out, "  stale snapshot: %s\n", what);
    }
}

static WalkAction on_snapshot_file(const char *rel, bool is_dir, void *ud) {
    SnapCheck *sc = (SnapCheck *)ud;
    size_t n = strlen(rel);
    if (is_dir || plat_is_tmp_name(rel) || n < 7 ||
        strcmp(rel + n - 6, ".jsonl") != 0)
        return WALK_CONT;
    char file[LAP_PATH_MAX];
    snprintf(file, sizeof file, "%.*s", (int)(n - 6), rel);
    char path[LAP_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", sc->snapdir, rel);
    char *data;
    size_t len;
    if (!plat_read_file_max(sc->a, path, &data, &len, (size_t)-1))
        return WALK_CONT;
    Lines l = split_lines(sc->a, data, len);
    for (int32_t i = 0; i < l.count; i++) {
        if (l.lines[i].len == 0)
            continue;
        sc->dc->checked++;
        char err[128];
        JVal *v = json_parse(sc->a, l.lines[i].ptr, l.lines[i].len, err,
                             sizeof err);
        int64_t at = v ? jobj_int(v, "at", -1) : -1;
        JVal *content = v ? jobj_get(v, "content") : NULL;
        Lines st;
        bool deleted;
        if (at < 0 || !content || content->t != J_STR ||
            !rec_replay_file(sc->a, sc->log, file, (int32_t)at, &st,
                             &deleted) ||
            deleted) {
            snap_flag_mismatch(sc, file);
            continue;
        }
        size_t blen;
        char *bytes = join_lines(sc->a, st, &blen);
        if (blen != content->s.len ||
            memcmp(bytes, content->s.ptr, blen) != 0)
            snap_flag_mismatch(sc, file);
    }
    return WALK_CONT;
}

int32_t cmd_verify(Arena *a, int32_t argc, char **argv) {
    static const char *const bool_flags[] = {"--json", "--deep", NULL};
    bool json = has_flag(argc, argv, NULL, "--json");
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    bool deep = has_flag(argc, argv, NULL, "--deep");

    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    RecLog log;
    if (!rec_log_load(a, repo.logpath, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }

    StrBuf deep_out;
    sb_init(&deep_out, a);
    int32_t checked = 0, mismatched = 0;
    if (deep) {
        DeepCheck dc;
        memset(&dc, 0, sizeof dc);
        dc.a = a;
        dc.log = &log;
        dc.repo = &repo;
        dc.out = &deep_out;
        dc.json = json;
        char shadow_root[LAP_PATH_MAX];
        snprintf(shadow_root, sizeof shadow_root, "%s/%s", repo.lapdir,
                 LAP_SHADOW_NAME);
        plat_walk(a, shadow_root, on_shadow_file, &dc);
        /* The walk covers files that HAVE a shadow. Also verify the other
         * direction: every file the log says exists must have one — a
         * missing shadow (failed write, interrupted commit) is invisible to
         * the walk. */
        StrSet seen;
        strset_init(&seen, a);
        for (int32_t i = 0; i < log.count; i++) {
            const Rec *rec = &log.v[i];
            if (rec->type != REC_COMMIT || !strset_add(&seen, rec->file))
                continue;
            Lines final_state;
            bool deleted;
            if (!rec_replay_file(a, &log, rec->file, log.count - 1,
                                 &final_state, &deleted) ||
                deleted)
                continue;
            char spath[LAP_PATH_MAX];
            snprintf(spath, sizeof spath, "%s/%s", shadow_root, rec->file);
            if (!plat_is_file(spath)) {
                dc.checked++;
                dc.mismatched++;
                if (json) {
                    if (dc.mismatched > 1)
                        sb_putc(&deep_out, ',');
                    json_escape_c(&deep_out, rec->file);
                } else {
                    sb_printf(&deep_out, "  missing shadow: %s\n",
                              rec->file);
                }
            }
        }
        /* snapshots are derived: each must equal a replay of the truth */
        char snapdir[LAP_PATH_MAX];
        snprintf(snapdir, sizeof snapdir, "%s/snapshots", repo.lapdir);
        SnapCheck sc = {a, &log, &dc, snapdir};
        plat_walk(a, snapdir, on_snapshot_file, &sc);
        checked = dc.checked;
        mismatched = dc.mismatched;
    }

    bool ok = log.chain_ok && mismatched == 0;
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb,
                  "{\"ok\":%s,\"records\":%d,\"chain_ok\":%s",
                  ok ? "true" : "false", log.count,
                  log.chain_ok ? "true" : "false");
        if (!log.chain_ok) {
            sb_puts(&sb, ",\"chain_error\":");
            json_escape_c(&sb, log.chain_err);
        }
        if (log.torn_tail)
            sb_printf(&sb, ",\"torn_tail_bytes\":%llu",
                      (unsigned long long)log.torn_bytes);
        if (deep) {
            sb_printf(&sb, ",\"deep_checked\":%d,\"deep_mismatched\":%d",
                      checked, mismatched);
            sb_puts(&sb, ",\"mismatched_files\":[");
            sb_putn(&sb, deep_out.data ? deep_out.data : "", deep_out.len);
            sb_puts(&sb, "]");
        }
        sb_puts(&sb, "}");
        puts(sb_finish(&sb));
    } else {
        if (log.chain_ok)
            printf("%schain ok%s: %d records\n", sgr(S_ADDED), sgr_off(),
                   log.count);
        else
            printf("%sCHAIN BROKEN%s: %s\n", sgr(S_REMOVED), sgr_off(),
                   log.chain_err);
        if (log.torn_tail)
            printf("note: torn trailing record ignored (%llu bytes from an "
                   "interrupted append; the next commit repairs it)\n",
                   (unsigned long long)log.torn_bytes);
        if (deep) {
            fputs(sb_finish(&deep_out), stdout);
            printf("deep check: %d file%s, %s%d mismatch%s%s\n", checked,
                   checked == 1 ? "" : "s",
                   sgr(mismatched ? S_REMOVED : S_ADDED), mismatched,
                   mismatched == 1 ? "" : "es", sgr_off());
        }
    }
    return ok ? LAP_EXIT_OK : LAP_EXIT_ERR;
}
