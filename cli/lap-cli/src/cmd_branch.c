#include "branches.h"
#include "cmd.h"
#include "sha256.h"

#include <time.h>

/* lap branch: a folder becomes a line of history of its own, started from
 * another folder's (its parent's) head. See SPEC.md, Branches. */

/* A name people type: letters, digits, '.', '_' and '-', not "main". */
static bool name_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 64 || strcmp(s, LAP_MAIN_LINEAGE) == 0 || s[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

/* The first 12 hex digits of SHA-256 over base, name, time and a nonce:
 * two copies of one folder never make the same id. */
static void new_branch_id(const char *base, const char *name, const char *ts,
                          char out[13]) {
    static int32_t salt;
    char buf[512];
    snprintf(buf, sizeof buf, "%s\n%s\n%s\n%ld\n%p\n%d\n%lld", base, name, ts,
             (long)clock(), (void *)&salt, ++salt,
             (long long)plat_now_sec());
    char hex[65];
    sha256_hex(buf, strlen(buf), hex);
    memcpy(out, hex, 12);
    out[12] = '\0';
}

typedef struct {
    Arena *a;
    const Ignore *ig;
    StrSet *seen;
    const char ***v;
    size_t *n, *cap;
} Collect;

static void collect_push(Collect *c, const char *rel) {
    if (!strset_add(c->seen, rel))
        return;
    ARENA_GROW(c->a, *c->v, *c->n, *c->cap, const char *);
    (*c->v)[(*c->n)++] = arena_strdup(c->a, rel);
}

static WalkAction on_work_entry(const char *rel, bool is_dir,
                                const PlatStat *st, void *ud) {
    (void)st;
    Collect *c = (Collect *)ud;
    if (ignore_match(c->ig, rel, is_dir))
        return is_dir ? WALK_SKIP_DIR : WALK_CONT;
    if (!is_dir)
        collect_push(c, rel);
    return WALK_CONT;
}

static WalkAction on_shadow_file(const char *rel, bool is_dir,
                                 const PlatStat *st, void *ud) {
    (void)st;
    if (!is_dir && !plat_is_tmp_name(rel))
        collect_push((Collect *)ud, rel);
    return WALK_CONT;
}

static int cmp_str(const void *pa, const void *pb) {
    return strcmp(*(const char *const *)pa, *(const char *const *)pb);
}

/* The files of folder `work` that differ from the parent's committed state
 * (`parent`'s shadow, or its history where a shadow is missing), as status
 * would report them there: new, modified or deleted. */
static int32_t pending_against(Arena *a, Repo *parent, const char *work,
                               const char ***out) {
    Repo view = *parent; /* the parent's history, this folder's files */
    snprintf(view.root, sizeof view.root, "%s", work);
    StrSet seen;
    strset_init(&seen, a);
    const char **v = NULL;
    size_t n = 0, cap = 0;
    Collect c = {a, ignore_load(a, work), &seen, &v, &n, &cap};
    plat_walk(a, work, on_work_entry, &c);
    char shadow_root[LAP_PATH_MAX];
    snprintf(shadow_root, sizeof shadow_root, "%s/%s", parent->lapdir,
             LAP_SHADOW_NAME);
    plat_walk(a, shadow_root, on_shadow_file, &c);
    if (n > 1)
        qsort(v, n, sizeof *v, cmp_str);
    const char **bad = NULL;
    size_t nb = 0, bcap = 0;
    Arena *fa = arena_new(1 << 16);
    for (size_t i = 0; i < n; i++) {
        arena_reset(fa);
        FileDiff fd;
        char err[256];
        if (!file_diff_load(fa, &view, v[i], &fd, err, sizeof err))
            continue;
        bool differs = fd.binary ? fd.shadow_exists
                       : fd.work_exists != fd.shadow_exists ||
                           fd.regions.count > 0;
        if (differs) {
            ARENA_GROW(a, bad, nb, bcap, const char *);
            bad[nb++] = v[i];
        }
    }
    arena_free(fa);
    *out = bad;
    return (int32_t)nb;
}

/* The index of the last chunk holding anything: where the base ends. */
static int32_t last_filled(const Hist *h) {
    for (int32_t i = h->n - 1; i >= 0; i--) {
        if (h->v[i].size > 0)
            return i;
    }
    return -1;
}

static int32_t branch_start(Arena *a, int32_t argc, char **argv, bool json) {
    static const char *const value_flags[] = {"--from", NULL};
    static const char *const bool_flags[] = {"--json", NULL};
    if (!flags_known(argc, argv, value_flags, bool_flags))
        return LAP_EXIT_ERR;
    const char *from = flag_value(argc, argv, value_flags, "--from");
    const char *name = positional_arg(argc, argv, value_flags, 1);
    if (!from) {
        err_out(json, "missing_from",
                "say which folder this branch starts from: lap branch start "
                "[name] --from <parent folder>");
        return LAP_EXIT_ERR;
    }
    if (positional_arg(argc, argv, value_flags, 2)) {
        err_out(json, "bad_args", "branch start takes at most one name");
        return LAP_EXIT_ERR;
    }
    if (name && !name_ok(name)) {
        err_out(json, "bad_name",
                "a branch name is 1-64 letters, digits, '.', '_' or '-', "
                "and not \"main\": \"%s\"",
                name);
        return LAP_EXIT_ERR;
    }
    char here[LAP_PATH_MAX], there[LAP_PATH_MAX];
    if (!repo_abspath(".", here, sizeof here) ||
        !repo_abspath(from, there, sizeof there)) {
        err_out(json, "bad_path", "cannot resolve %s", from);
        return LAP_EXIT_ERR;
    }
    if (strcmp(here, there) == 0) {
        err_out(json, "same_folder",
                "a folder cannot be a branch of itself: run this in the new "
                "folder, with --from naming the one it starts from");
        return LAP_EXIT_ERR;
    }
    char here_lap[LAP_PATH_MAX];
    snprintf(here_lap, sizeof here_lap, "%s/%s", here, LAP_DIR);
    char err[1024];
    char lineage[HIST_LINEAGE_MAX];
    if (!hist_folder_lineage(a, here_lap, lineage, err, sizeof err)) {
        err_out(json, "bad_lineage", "%s", err);
        return LAP_EXIT_ERR;
    }
    if (strcmp(lineage, LAP_MAIN_LINEAGE) != 0) {
        err_out(json, "already_branch",
                "%s is already branch %s; a folder starts one branch", here,
                lineage);
        return LAP_EXIT_ERR;
    }

    /* The parent, locked for the whole start: nothing lands on it between
     * the checks and the sealing. */
    Repo pr;
    if (!repo_open_at(a, &pr, there, true, err, sizeof err)) {
        err_out(json, "no_parent", "%s", err);
        return LAP_EXIT_ERR;
    }
    if (pr.hist.parent[0]) {
        err_out(json, "nested_branch",
                "%s is itself branch %s; branches start from a main folder "
                "in this version",
                there, pr.hist.name);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }

    /* 1. This folder's history, if it has one, must be where the parent's
     * was: its main chunks (or single-file log) a prefix of the parent's. */
    Hist hh;
    char *mine = NULL, *theirs = NULL;
    size_t mine_len = 0, theirs_len = 0;
    if (!hist_open(a, here_lap, LAP_MAIN_LINEAGE, &hh, err, sizeof err) ||
        !hist_read_all(a, &hh, &mine, &mine_len) ||
        !hist_read_all(a, &pr.hist, &theirs, &theirs_len)) {
        err_out(json, "log_unreadable", "%s", err[0] ? err : "cannot read "
                                                             "the history");
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }
    if (mine_len > theirs_len || memcmp(mine, theirs, mine_len) != 0) {
        err_out(json, "unrelated_history",
                "the history in %s is not part of %s's: a branch starts "
                "from a folder whose history it shares",
                here, there);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }

    /* 2. Its files must be the parent's committed state: the base. */
    const char **bad;
    int32_t nbad = pending_against(a, &pr, here, &bad);
    if (nbad > 0) {
        StrBuf sb;
        sb_init(&sb, a);
        for (int32_t i = 0; i < nbad && i < 20; i++)
            sb_printf(&sb, "%s%s", i ? ", " : "", bad[i]);
        if (nbad > 20)
            sb_printf(&sb, " and %d more", nbad - 20);
        err_out(json, "not_clean",
                "%d file%s here differ%s from %s's last commit: %s. A branch "
                "starts from the parent's committed state. Usual causes: the "
                "worktree was checked out from a git commit older than lap's "
                "history (git-commit the parent's work first), or lap tracks "
                "files git ignores (copy them over)",
                nbad, nbad == 1 ? "" : "s", nbad == 1 ? "s" : "", there,
                sb_finish(&sb));
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }

    Branches reg;
    branches_load(a, pr.lapdir, &reg);
    if (name && branches_find(&reg, name)) {
        err_out(json, "name_taken",
                "%s already has a branch named %s; pick another name", there,
                name);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }

    /* 3. Seal the parent's open chunk: the chunk this folder copies is
     * then final on both sides, and a later git merge finds it unchanged. */
    if (!hist_seal(a, &pr.hist, err, sizeof err)) {
        err_out(json, "parent_read_only",
                "cannot write the parent's history (%s); a branch needs its "
                "parent writable",
                err);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }
    int32_t filled = last_filled(&pr.hist);
    int32_t base_chunk = pr.hist.v[filled].n;
    char ts[32];
    plat_timestamp(ts);
    char id[13];
    do
        new_branch_id(pr.last_hash, name ? name : "", ts, id);
    while (branches_find(&reg, id));
    const char *bname = name ? name : id;

    /* 5. Registration, in the parent, before anything is written here: a
     * parent that cannot take it leaves this folder untouched. */
    BranchEntry e = {arena_strdup(a, id), arena_strdup(a, bname),
                     arena_strdup(a, here), arena_strdup(a, pr.last_hash),
                     arena_strdup(a, ts)};
    branches_add(a, &reg, e);
    if (!branches_save(a, pr.lapdir, &reg)) {
        err_out(json, "parent_read_only",
                "cannot write %s/%s; a branch needs its parent writable",
                pr.lapdir, LAP_BRANCHES_NAME);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }

    /* 1 (continued). The parent's chunks up to the base, here. */
    char here_log[LAP_PATH_MAX];
    snprintf(here_log, sizeof here_log, "%s/%s", here_lap, LAP_LOG_DIR);
    if (!plat_mkdirs(here_log)) {
        err_out(json, "io_error", "cannot create %s", here_log);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }
    for (int32_t i = 0; i <= filled; i++) {
        const HistChunk *k = &pr.hist.v[i];
        char path[LAP_PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", here_log, k->name);
        if (!plat_write_file_atomic(path, theirs + k->start, (size_t)k->size)) {
            err_out(json, "io_error", "cannot write %s", path);
            repo_close(&pr);
            return LAP_EXIT_ERR;
        }
    }
    if (hh.legacy) {
        char legacy[LAP_PATH_MAX];
        snprintf(legacy, sizeof legacy, "%s/%s", here_lap, LAP_LOG_NAME);
        plat_remove_file(legacy);
    }
    /* a copied folder brought the parent's registry: those are the
     * parent's branches, not this folder's */
    char copied_reg[LAP_PATH_MAX];
    snprintf(copied_reg, sizeof copied_reg, "%s/%s", here_lap,
             LAP_BRANCHES_NAME);
    if (plat_is_file(copied_reg))
        plat_remove_file(copied_reg);

    /* 4. The branch record opens this folder's own lineage. */
    Rec rec;
    memset(&rec, 0, sizeof rec);
    rec.type = REC_BRANCH;
    rec.id = id;
    rec.name = bname;
    rec.parent = LAP_MAIN_LINEAGE;
    rec.base = pr.last_hash;
    rec.base_chunk = base_chunk;
    rec.user = repo_user(&pr);
    rec.ts = ts;
    rec.prev = pr.last_hash;
    size_t len;
    char *line = rec_encode(a, &rec, &len);
    char chunk[64], path[LAP_PATH_MAX];
    hist_chunk_name(id, 1, chunk);
    snprintf(path, sizeof path, "%s/%s", here_log, chunk);
    char *with_nl = arena_printf(a, "%s\n", line);
    if (!plat_write_file_atomic(path, with_nl, len + 1) ||
        !hist_write_lineage(here_lap, id)) {
        err_out(json, "io_error", "cannot write the branch record in %s",
                here_log);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }
    char short_base[8];
    snprintf(short_base, sizeof short_base, "%.7s", pr.last_hash);
    repo_close(&pr);

    /* Caches here are built from the history just written. */
    Repo hr;
    if (!repo_open_at(a, &hr, here, true, err, sizeof err) ||
        !repo_rebuild(a, &hr, err, sizeof err)) {
        err_out(json, "io_error", "branch started, but its caches were not "
                                  "built (%s); run lap rebuild",
                err);
        return LAP_EXIT_ERR;
    }
    repo_close(&hr);

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb, "{\"ok\":true,\"id\":\"%s\",\"name\":", id);
        json_escape_c(&sb, bname);
        sb_puts(&sb, ",\"parent\":");
        json_escape_c(&sb, there);
        sb_printf(&sb, ",\"base\":\"%s\",\"base_chunk\":%d}", rec.base,
                  base_chunk);
        puts(sb_finish(&sb));
    } else {
        printf("branch %s (%s) started from %s at %s\n", bname, id, there,
               short_base);
    }
    return LAP_EXIT_OK;
}

static int32_t branch_list(Arena *a, int32_t argc, char **argv, bool json) {
    static const char *const bool_flags[] = {"--json", NULL};
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    RecLog log;
    if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }
    Branches reg;
    branches_load(a, repo.lapdir, &reg);
    StrBuf sb;
    sb_init(&sb, a);
    const Hist *h = &repo.hist;
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"self\":");
        if (h->parent[0]) {
            sb_printf(&sb, "{\"id\":\"%s\",\"name\":", h->lineage);
            json_escape_c(&sb, h->name);
            sb_printf(&sb, ",\"parent\":\"%s\",\"base\":\"%s\"}", h->parent,
                      h->base);
        } else {
            sb_puts(&sb, "null");
        }
        sb_puts(&sb, ",\"branches\":[");
    } else if (h->parent[0]) {
        sb_printf(&sb, "this folder is branch %s (%s) of %s, from %.7s\n",
                  h->name, h->lineage, h->parent, h->base);
    }
    for (int32_t i = 0; i < reg.n; i++) {
        const BranchEntry *e = &reg.v[i];
        BranchStatus st;
        branches_status(a, repo.lapdir, &log, e, &st);
        if (json) {
            sb_puts(&sb, i ? ",{\"id\":" : "{\"id\":");
            json_escape_c(&sb, e->id);
            sb_puts(&sb, ",\"name\":");
            json_escape_c(&sb, e->name);
            sb_printf(&sb, ",\"state\":\"%s\",\"present\":%s,\"path\":",
                      st.state, st.present ? "true" : "false");
            json_escape_c(&sb, e->path);
            sb_printf(&sb, ",\"base\":\"%s\",\"started\":\"%s\"", e->base,
                      e->started);
            if (st.readable)
                sb_printf(&sb, ",\"since_base\":%d,\"since_merge\":%d",
                          st.since_base, st.since_merge);
            else
                sb_puts(&sb, ",\"since_base\":null,\"since_merge\":null");
            sb_puts(&sb, ",\"merged\":");
            if (st.merged)
                sb_printf(&sb, "\"%s\"", st.merged);
            else
                sb_puts(&sb, "null");
            sb_puts(&sb, ",\"stopped\":[");
            for (int32_t k = 0; k < st.nstopped; k++) {
                if (k)
                    sb_putc(&sb, ',');
                json_escape_c(&sb, st.stopped[k]);
            }
            sb_puts(&sb, "]}");
            continue;
        }
        sb_printf(&sb, "%-16s %-14s %s%s\n", e->name, st.state, e->path,
                  st.present ? "" : " (gone)");
        if (st.readable)
            sb_printf(&sb, "  %d commit%s since its base, %d since the last "
                           "merge\n",
                      st.since_base, st.since_base == 1 ? "" : "s",
                      st.since_merge);
        for (int32_t k = 0; k < st.nstopped; k++)
            sb_printf(&sb, "  stopped: %s\n", st.stopped[k]);
        if (strcmp(st.state, "missing") == 0)
            sb_printf(&sb, "  its folder is gone: lap branch move %s <path> "
                           "if it moved, lap branch forget %s if it is no "
                           "more\n",
                      e->name, e->name);
    }
    if (json) {
        sb_puts(&sb, "]}");
        puts(sb_finish(&sb));
    } else {
        if (reg.n == 0 && !h->parent[0])
            sb_puts(&sb, "no branches started from this folder\n");
        fputs(sb_finish(&sb), stdout);
    }
    return LAP_EXIT_OK;
}

/* forget and move: the registry changed under the lock. */
static int32_t branch_edit(Arena *a, int32_t argc, char **argv, bool json,
                           bool move) {
    static const char *const bool_flags[] = {"--json", NULL};
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    const char *key = positional_arg(argc, argv, NULL, 1);
    const char *to = positional_arg(argc, argv, NULL, 2);
    if (!key || (move && !to) || positional_arg(argc, argv, NULL, move ? 3 : 2)) {
        err_out(json, "bad_args", move ? "usage: lap branch move <branch> "
                                         "<path>"
                                       : "usage: lap branch forget <branch>");
        return LAP_EXIT_ERR;
    }
    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, true, err, sizeof err)) {
        err_out(json, "no_repo", "%s", err);
        return LAP_EXIT_ERR;
    }
    int32_t rc = LAP_EXIT_ERR;
    Branches reg;
    branches_load(a, repo.lapdir, &reg);
    BranchEntry *e = (BranchEntry *)branches_find(&reg, key);
    char path[LAP_PATH_MAX];
    if (!e) {
        err_out(json, "unknown_branch",
                "no branch %s in this folder's registry", key);
        goto done;
    }
    if (move) {
        if (!repo_abspath(to, path, sizeof path) ||
            !branch_folder_is(a, path, e->id)) {
            err_out(json, "not_that_branch",
                    "%s does not hold branch %s: its .lap/lineage must name "
                    "%s",
                    to, e->name, e->id);
            goto done;
        }
        e->path = path;
    } else {
        *e = reg.v[reg.n - 1];
        reg.n--;
    }
    if (!branches_save(a, repo.lapdir, &reg)) {
        err_out(json, "io_error", "cannot write %s/%s", repo.lapdir,
                LAP_BRANCHES_NAME);
        goto done;
    }
    if (json)
        printf("{\"ok\":true}\n");
    else if (move)
        printf("branch %s now at %s\n", key, path);
    else
        printf("forgot branch %s\n", key);
    rc = LAP_EXIT_OK;
done:
    repo_close(&repo);
    return rc;
}

int32_t cmd_branch(Arena *a, int32_t argc, char **argv) {
    static const char *const value_flags[] = {"--from", NULL};
    bool json = has_flag(argc, argv, value_flags, "--json");
    const char *sub = positional_arg(argc, argv, value_flags, 0);
    if (sub && strcmp(sub, "start") == 0)
        return branch_start(a, argc, argv, json);
    if (sub && strcmp(sub, "list") == 0)
        return branch_list(a, argc, argv, json);
    if (sub && strcmp(sub, "forget") == 0)
        return branch_edit(a, argc, argv, json, false);
    if (sub && strcmp(sub, "move") == 0)
        return branch_edit(a, argc, argv, json, true);
    err_out(json, "bad_args",
            "usage: lap branch start [name] --from <parent folder> | list | "
            "forget <branch> | move <branch> <path>");
    return LAP_EXIT_ERR;
}
