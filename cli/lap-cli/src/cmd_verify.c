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

static WalkAction on_shadow_file(const char *rel, bool is_dir,
                                 const PlatStat *st, void *ud) {
    (void)st;
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

static WalkAction on_snapshot_file(const char *rel, bool is_dir,
                                   const PlatStat *st, void *ud) {
    (void)st;
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

/* The files under a folder, as paths relative to it. */
typedef struct {
    Arena *a;
    const char **rels;
    size_t n, cap;
} RelList;

static WalkAction collect_rel(const char *rel, bool is_dir,
                              const PlatStat *st, void *ud) {
    (void)st;
    RelList *rl = (RelList *)ud;
    if (!is_dir && !plat_is_tmp_name(rel)) {
        ARENA_GROW(rl->a, rl->rels, rl->n, rl->cap, const char *);
        rl->rels[rl->n++] = arena_strdup(rl->a, rel);
    }
    return WALK_CONT;
}

/* One snapshot line of a file: where it is in the snapshot file and the
 * entry it was taken at; its content is read only when it is checked. */
typedef struct {
    uint64_t off;
    size_t len;
    int64_t at;
} SnapLine;

static int cmp_snap(const void *pa, const void *pb) {
    const SnapLine *x = (const SnapLine *)pa, *y = (const SnapLine *)pb;
    return x->at < y->at ? -1 : x->at > y->at;
}

static void deep_flag(DeepCheck *dc, const char *fmt, const char *rel) {
    dc->mismatched++;
    if (dc->json) {
        if (dc->mismatched > 1)
            sb_putc(dc->out, ',');
        json_escape_c(dc->out, rel);
    } else {
        sb_printf(dc->out, fmt, rel);
    }
}

/* A snapshot file's lines, found by reading it a block at a time: offsets,
 * lengths and each one's `at` (-1 when it is not a line lap writes). */
static SnapLine *snap_lines(Arena *a, const char *path, int32_t *n) {
    enum { BLOCK = 1 << 20 };
    *n = 0;
    uint64_t size;
    if (!plat_file_size(path, &size) || size == 0)
        return NULL;
    SnapLine *v = NULL;
    size_t cap = 0, count = 0;
    Arena *ba = arena_new(1 << 16);
    uint64_t line_start = 0;
    for (uint64_t off = 0; off < size; off += BLOCK) {
        size_t len = size - off < BLOCK ? (size_t)(size - off) : BLOCK;
        char *data;
        arena_reset(ba);
        if (!plat_read_range(ba, path, off, len, &data))
            break;
        for (size_t i = 0; i < len; i++) {
            if (data[i] != '\n')
                continue;
            uint64_t end = off + i;
            if (end > line_start) {
                ARENA_GROW(a, v, count, cap, SnapLine);
                v[count++] = (SnapLine){line_start,
                                        (size_t)(end - line_start), -1};
            }
            line_start = end + 1;
        }
    }
    arena_free(ba);
    /* each line's `at`, from its first bytes: {"at":N,... */
    Arena *ha = arena_new(1 << 12);
    for (size_t i = 0; i < count; i++) {
        char *head;
        size_t hl = v[i].len < 40 ? v[i].len : 40;
        arena_reset(ha);
        if (plat_read_range(ha, path, v[i].off, hl, &head) && hl > 6 &&
            memcmp(head, "{\"at\":", 6) == 0)
            v[i].at = strtoll(head + 6, NULL, 10);
    }
    arena_free(ha);
    *n = (int32_t)count;
    return v;
}

/* Whether snapshot s of the file at path holds state l. */
static bool snap_holds(Arena *a, const char *path, const SnapLine *s,
                       Lines l) {
    char *line;
    char err[128];
    if (s->at < 0 || !plat_read_range(a, path, s->off, s->len, &line))
        return false;
    JVal *j = json_parse(a, line, s->len, err, sizeof err);
    JVal *content = j ? jobj_get(j, "content") : NULL;
    if (!content || content->t != J_STR)
        return false;
    size_t n;
    char *joined = join_lines(a, l, &n);
    return n == content->s.len && memcmp(joined, content->s.ptr, n) == 0;
}

static bool lines_equal(Arena *a, Lines l, Str bytes) {
    size_t n;
    char *joined = join_lines(a, l, &n);
    return n == bytes.len && memcmp(joined, bytes.ptr, n) == 0;
}

/* --deep through the index: each file replayed once along its own chain,
 * in an arena of its own (compacted as it grows), its snapshots checked on
 * the way and its final state against its shadow. Memory is one file's,
 * not the history's, and the results are the whole-log check's. */
static void deep_indexed(Arena *a, Repo *repo, Idx *idx, DeepCheck *dc) {
    enum { COMPACT_EVERY = 256 };
    char shadow_root[LAP_PATH_MAX], snapdir[LAP_PATH_MAX];
    snprintf(shadow_root, sizeof shadow_root, "%s/%s", repo->lapdir,
             LAP_SHADOW_NAME);
    snprintf(snapdir, sizeof snapdir, "%s/snapshots", repo->lapdir);
    /* files with a shadow, and what their check found */
    RelList shadows = {a, NULL, 0, 0};
    plat_walk(a, shadow_root, collect_rel, &shadows);
    StrSet with_shadow, live_ok, live;
    strset_init(&with_shadow, a);
    strset_init(&live_ok, a);
    strset_init(&live, a);
    for (size_t i = 0; i < shadows.n; i++)
        strset_add(&with_shadow, shadows.rels[i]);
    for (int32_t fid = 0; fid < idx->npaths; fid++) {
        const char *rel = idx->paths[fid];
        Arena *fa = arena_new(1 << 16);
        char spath[LAP_PATH_MAX];
        snprintf(spath, sizeof spath, "%s/%s.jsonl", snapdir, rel);
        int32_t ns;
        SnapLine *snaps = snap_lines(fa, spath, &ns);
        if (ns > 1)
            qsort(snaps, (size_t)ns, sizeof *snaps, cmp_snap);
        int64_t *chain = NULL;
        size_t n = 0, cap = 0;
        for (int64_t e = idx->heads[fid].head; e >= 0;
             e = idx->v[e].prev_same_file) {
            ARENA_GROW(a, chain, n, cap, int64_t);
            chain[n++] = e;
        }
        Lines cur = {NULL, 0, true};
        bool exists = false, fetched_all = true;
        int32_t s = 0;
        for (size_t i = n; i > 0; i--) {
            int64_t e = chain[i - 1];
            /* snapshots taken before this record see the state so far */
            for (; s < ns && snaps[s].at < e; s++) {
                dc->checked++;
                if (!exists || !snap_holds(fa, spath, &snaps[s], cur))
                    deep_flag(dc, "  stale snapshot: %s\n", rel);
            }
            Rec rec;
            if (!idx_fetch(fa, repo, idx, e, &rec)) {
                fetched_all = false;
                break;
            }
            rec_apply(fa, &cur, &rec);
            exists = idx->v[e].op != IDX_OP_DELETE;
            if ((n - i + 1) % COMPACT_EVERY == 0) {
                /* the snapshot list moves along */
                Arena *fresh = arena_new(1 << 16);
                cur = lines_copy(fresh, cur);
                SnapLine *moved = (SnapLine *)arena_alloc(
                    fresh, (size_t)(ns ? ns : 1) * sizeof(SnapLine));
                memcpy(moved, snaps, (size_t)ns * sizeof(SnapLine));
                snaps = moved;
                arena_free(fa);
                fa = fresh;
            }
        }
        for (; s < ns; s++) { /* taken at or after the last record */
            dc->checked++;
            if (!exists || !fetched_all ||
                !snap_holds(fa, spath, &snaps[s], cur))
                deep_flag(dc, "  stale snapshot: %s\n", rel);
        }
        if (exists && fetched_all) {
            strset_add(&live, rel);
            char *sdata;
            size_t slen;
            snprintf(spath, sizeof spath, "%s/%s", shadow_root, rel);
            if (plat_read_file(fa, spath, &sdata, &slen) &&
                lines_equal(fa, cur, (Str){sdata, slen}))
                strset_add(&live_ok, rel);
        }
        arena_free(fa);
    }
    /* every shadow must be a live file's final state */
    for (size_t i = 0; i < shadows.n; i++) {
        dc->checked++;
        if (!strset_has(&live_ok, shadows.rels[i]))
            deep_flag(dc, "  shadow/log mismatch: %s\n", shadows.rels[i]);
    }
    /* and every live file must have one */
    for (int32_t fid = 0; fid < idx->npaths; fid++) {
        const char *rel = idx->paths[fid];
        if (strset_has(&live, rel) && !strset_has(&with_shadow, rel)) {
            dc->checked++;
            deep_flag(dc, "  missing shadow: %s\n", rel);
        }
    }
    /* snapshots of files the history does not know */
    RelList snapfiles = {a, NULL, 0, 0};
    plat_walk(a, snapdir, collect_rel, &snapfiles);
    for (size_t i = 0; i < snapfiles.n; i++) {
        size_t len = strlen(snapfiles.rels[i]);
        if (len < 7 || strcmp(snapfiles.rels[i] + len - 6, ".jsonl") != 0)
            continue;
        char *rel = arena_strndup(a, snapfiles.rels[i], len - 6);
        if (idx_file_id(idx, rel) >= 0)
            continue;
        char path[LAP_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", snapdir, snapfiles.rels[i]);
        int32_t ns;
        snap_lines(a, path, &ns);
        for (int32_t k = 0; k < ns; k++) {
            dc->checked++;
            deep_flag(dc, "  stale snapshot: %s\n", rel);
        }
    }
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
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    /* A pass a chunk at a time answers a sound history; the whole log is
     * read only to name a broken chain's blame, or for --deep where no
     * index can lead the replay. */
    HistScan scan;
    if (!hist_scan(a, &repo.hist, &scan, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }
    Idx *ix = deep && scan.chain_ok ? idx_ready(a, &repo) : NULL;
    bool whole = !scan.chain_ok || (deep && !ix);
    RecLog log;
    memset(&log, 0, sizeof log);
    if (whole) {
        if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
            err_out(json, "log_unreadable", "%s", err);
            return LAP_EXIT_ERR;
        }
    } else {
        log.count = scan.records;
        log.chain_ok = true;
        log.chain_break_index = -1;
        log.torn_tail = scan.torn_bytes > 0;
        log.torn_bytes = scan.torn_bytes;
        log.unknown_n = scan.unknown_n;
        log.unknown_type = scan.unknown_type;
        if (scan.unknown_n > 0)
            rec_note_newer(scan.unknown_type);
    }

    StrBuf deep_out;
    sb_init(&deep_out, a);
    int32_t checked = 0, mismatched = 0;
    if (deep && !whole) {
        DeepCheck dc;
        memset(&dc, 0, sizeof dc);
        dc.a = a;
        dc.repo = &repo;
        dc.out = &deep_out;
        dc.json = json;
        deep_indexed(a, &repo, ix, &dc);
        checked = dc.checked;
        mismatched = dc.mismatched;
    } else if (deep) {
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

    /* Other branches whose chunks are here (git merge brought them, or lap
     * merge copied them): their chains are walked too, each from its base
     * in this folder's chunks. */
    StrBuf others_json, others_text;
    sb_init(&others_json, a);
    sb_init(&others_text, a);
    bool others_ok = true;
    const char **lineages;
    int32_t nl = hist_lineages(a, repo.lapdir, &lineages);
    for (int32_t i = 0; i < nl; i++) {
        if (strcmp(lineages[i], repo.hist.lineage) == 0)
            continue;
        Repo other = repo;
        RecLog olog;
        memset(&olog, 0, sizeof olog);
        HistScan oscan;
        bool read = hist_open_lineage(a, repo.lapdir, lineages[i],
                                      &other.hist, err, sizeof err) &&
                    hist_scan(a, &other.hist, &oscan, err, sizeof err);
        if (read && oscan.chain_ok) {
            olog.count = oscan.records;
            olog.chain_ok = true;
        } else if (read) { /* the whole branch only to name its break */
            read = repo_log_load(a, &other, &olog, err, sizeof err);
        }
        bool chain = read && olog.chain_ok;
        others_ok = others_ok && chain;
        const char *name = read ? other.hist.name : lineages[i];
        const char *why = !read ? err : !olog.chain_ok ? olog.chain_err : "";
        sb_puts(&others_json, others_json.len ? ",{\"branch\":" : "{\"branch\":");
        json_escape_c(&others_json, name);
        sb_printf(&others_json, ",\"records\":%d,\"chain_ok\":%s",
                  read ? olog.count : 0, chain ? "true" : "false");
        if (!chain) {
            sb_puts(&others_json, ",\"chain_error\":");
            json_escape_c(&others_json, why);
        }
        sb_putc(&others_json, '}');
        if (chain)
            sb_printf(&others_text, "%sbranch %s: chain ok%s: %d records\n",
                      sgr(S_ADDED), name, sgr_off(), olog.count);
        else
            sb_printf(&others_text, "%sbranch %s: CHAIN BROKEN%s: %s\n",
                      sgr(S_REMOVED), name, sgr_off(), why);
    }

    bool ok = log.chain_ok && mismatched == 0 && others_ok;
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
        if (log.unknown_n > 0) {
            sb_printf(&sb, ",\"unknown_records\":%d,\"unknown_type\":",
                      log.unknown_n);
            json_escape_c(&sb, log.unknown_type);
        }
        if (deep) {
            sb_printf(&sb, ",\"deep_checked\":%d,\"deep_mismatched\":%d",
                      checked, mismatched);
            sb_puts(&sb, ",\"mismatched_files\":[");
            sb_putn(&sb, deep_out.data ? deep_out.data : "", deep_out.len);
            sb_puts(&sb, "]");
        }
        sb_puts(&sb, ",\"branches\":[");
        sb_putn(&sb, others_json.data ? others_json.data : "",
                others_json.len);
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        if (log.chain_ok)
            printf("%schain ok%s: %d records\n", sgr(S_ADDED), sgr_off(),
                   log.count);
        else
            printf("%sCHAIN BROKEN%s: %s\n", sgr(S_REMOVED), sgr_off(),
                   log.chain_err);
        fputs(others_text.len ? sb_finish(&others_text) : "", stdout);
        if (log.torn_tail)
            printf("note: torn trailing record ignored (%llu bytes from an "
                   "interrupted append; the next commit repairs it)\n",
                   (unsigned long long)log.torn_bytes);
        if (log.unknown_n > 0)
            printf("note: %d record%s of a type this lap does not know "
                   "(\"%s\"): in the chain checked above, not interpreted\n",
                   log.unknown_n, log.unknown_n == 1 ? "" : "s",
                   log.unknown_type);
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
