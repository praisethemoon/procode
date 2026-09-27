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
 * would report them there: new, modified or deleted. *fresh marks those the
 * parent never recorded at all (new to lap, however old to git). */
static int32_t pending_against(Arena *a, Repo *parent, const char *work,
                               const char ***out, bool **fresh) {
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
    bool *isnew = NULL;
    size_t nb = 0, bcap = 0, ncap = 0;
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
            size_t nn = nb;
            ARENA_GROW(a, bad, nb, bcap, const char *);
            ARENA_GROW(a, isnew, nn, ncap, bool);
            isnew[nb] = fd.work_exists && !fd.shadow_exists;
            bad[nb++] = v[i];
        }
    }
    arena_free(fa);
    *out = bad;
    *fresh = isnew;
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

/* branch start from its checks on, with this folder locked (here_lock,
 * released before its caches are built). */
static int32_t start_locked(Arena *a, bool json, const char *name,
                            const char *here, const char *there,
                            const char *here_lap, PlatLock **here_lock) {
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

    /* A start writes to the parent (its lock, a sealed chunk, the registry):
     * one it could not write to is refused before any of that, so a refusal
     * leaves the parent as it was. */
    char there_lap[LAP_PATH_MAX], there_log[LAP_PATH_MAX];
    snprintf(there_lap, sizeof there_lap, "%s/%s", there, LAP_DIR);
    snprintf(there_log, sizeof there_log, "%s/%s", there_lap, LAP_LOG_DIR);
    if (plat_is_dir(there_lap) &&
        (!plat_is_writable_dir(there_lap) ||
         (plat_is_dir(there_log) && !plat_is_writable_dir(there_log)))) {
        err_out(json, "parent_read_only",
                "cannot write %s; a branch needs its parent writable",
                plat_is_writable_dir(there_lap) ? there_log : there_lap);
        return LAP_EXIT_ERR;
    }
    /* The parent, locked for the whole start: nothing lands on it between
     * the checks and the sealing. */
    Repo pr;
    if (!repo_open_at(a, &pr, there, true, err, sizeof err)) {
        err_out(json, "no_parent", "%s", err);
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
    bool *fresh;
    int32_t nbad = pending_against(a, &pr, here, &bad, &fresh);
    if (nbad > 0) {
        /* files lap never recorded (new to it, however old to git) apart
         * from changed ones: each has its own way out */
        StrBuf nsb, csb;
        sb_init(&nsb, a);
        sb_init(&csb, a);
        int32_t nnew = 0, nchg = 0;
        for (int32_t i = 0; i < nbad; i++) {
            StrBuf *sb = fresh[i] ? &nsb : &csb;
            int32_t *k = fresh[i] ? &nnew : &nchg;
            if (*k < 20)
                sb_printf(sb, "%s%s", *k ? ", " : "", bad[i]);
            else if (*k == 20)
                sb_puts(sb, ", …");
            (*k)++;
        }
        StrBuf msg;
        sb_init(&msg, a);
        sb_printf(&msg,
                  "%d file%s here differ%s from %s's last commit. A branch "
                  "starts from the parent's committed state.",
                  nbad, nbad == 1 ? "" : "s", nbad == 1 ? "s" : "", there);
        if (nnew)
            sb_printf(&msg,
                      " Never recorded by lap there (%d): %s. Commit them "
                      "in the parent (lap commit <file>), or add them to "
                      "%s there if lap should not track them.",
                      nnew, sb_finish(&nsb), LAP_IGNORE_NAME);
        if (nchg)
            sb_printf(&msg,
                      " Changed from what lap recorded (%d): %s. Usual "
                      "causes: the worktree was checked out from a git "
                      "commit older than lap's history (git-commit the "
                      "parent's work first), or lap tracks files git "
                      "ignores (copy them over).",
                      nchg, sb_finish(&csb));
        err_out(json, "not_clean", "%s", sb_finish(&msg));
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }

    Branches reg;
    branches_load(a, pr.lapdir, &reg);
    /* A name stays taken once used here — in the registry, by a branch
     * whose chunks are here, or by one a merge recorded — so a name names
     * one branch for good. */
    bool taken = name && (branches_find(&reg, name) ||
                          branch_find(a, &pr, &reg, name));
    if (name && !taken) {
        RecLog plog;
        char perr[256];
        if (rec_log_parse(a, theirs, theirs_len, NULL, NULL, &plog, perr,
                          sizeof perr)) {
            for (int32_t i = 0; i < plog.count && !taken; i++)
                taken = plog.v[i].type == REC_MERGE &&
                        strcmp(plog.v[i].name, name) == 0;
        }
    }
    if (taken) {
        err_out(json, "name_taken",
                "%s already has, or had, a branch named %s: a name stays "
                "with one branch for good; pick another name",
                there, name);
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
                     arena_strdup(a, ts), NULL};
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
    /* this folder's lineage and parent never go through git; the parent,
     * if it predates that rule, gets the same guard */
    if (!repo_write_gitignore(here_lap)) {
        err_out(json, "io_error", "cannot write %s/.gitignore", here_lap);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }
    repo_write_gitignore(pr.lapdir); /* a hint there: a failure is harmless */
    for (int32_t i = 0; i <= filled; i++) {
        const HistChunk *k = &pr.hist.v[i];
        if (!hist_write_chunk(here_log, k->name, theirs + k->start,
                              (size_t)k->size)) {
            err_out(json, "io_error", "cannot write %s/%s", here_log, k->name);
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
    rec.parent = pr.hist.lineage; /* main, or the branch it starts from */
    rec.base = pr.last_hash;
    rec.base_chunk = base_chunk;
    rec.user = repo_user(&pr);
    rec.ts = ts;
    rec.prev = pr.last_hash;
    size_t len;
    char *line = rec_encode(a, &rec, &len);
    char chunk[64];
    hist_chunk_name(id, 1, chunk);
    char *with_nl = arena_printf(a, "%s\n", line);
    if (!hist_write_chunk(here_log, chunk, with_nl, len + 1) ||
        !hist_write_lineage(here_lap, id)) {
        err_out(json, "io_error", "cannot write the branch record in %s",
                here_log);
        repo_close(&pr);
        return LAP_EXIT_ERR;
    }
    /* where the parent is, for tools that serve the parent from here (the
     * board): a hint like the registry, never read by lap itself */
    char parent_file[LAP_PATH_MAX];
    snprintf(parent_file, sizeof parent_file, "%s/%s", here_lap,
             LAP_PARENT_NAME);
    char *parent_line = arena_printf(a, "%s\n", there);
    plat_write_file_atomic(parent_file, parent_line, strlen(parent_line));
    char short_base[8];
    snprintf(short_base, sizeof short_base, "%.7s", pr.last_hash);
    repo_close(&pr);

    /* Caches here are built from the history just written, under the
     * lock repo_open takes: this start's own is released first. */
    plat_unlock(*here_lock);
    *here_lock = NULL;
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
    /* one folder, however spelled (case, a symlink, ".."): the same
     * directory on disk */
    if (strcmp(here, there) == 0 || plat_same_file(here, there)) {
        err_out(json, "same_folder",
                "a folder cannot be a branch of itself: run this in the new "
                "folder, with --from naming the one it starts from");
        return LAP_EXIT_ERR;
    }
    char here_lap[LAP_PATH_MAX];
    snprintf(here_lap, sizeof here_lap, "%s/%s", here, LAP_DIR);
    /* This folder is locked for the whole start, so two starts in it
     * cannot both find it unbranched. A .lap made only for the lock goes
     * again when the start is refused. */
    bool made = !plat_is_dir(here_lap);
    char lockpath[LAP_PATH_MAX];
    snprintf(lockpath, sizeof lockpath, "%s/%s", here_lap, LAP_LOCK_NAME);
    PlatLock *here_lock = plat_mkdirs(here_lap) ? plat_lock(a, lockpath)
                                                : NULL;
    int32_t rc = LAP_EXIT_ERR;
    if (!here_lock)
        err_out(json, "io_error", "cannot lock %s", here_lap);
    else
        rc = start_locked(a, json, name, here, there, here_lap, &here_lock);
    if (here_lock)
        plat_unlock(here_lock);
    if (rc != LAP_EXIT_OK && made) {
        plat_remove_file(lockpath);
        plat_rmdir(here_lap); /* only when nothing else was written */
    }
    return rc;
}

/* True when lineage is main, or one whose chunks this folder's own history
 * reads (itself, and in a branch folder the branches it started from). */
static bool in_this_history(const Hist *h, const char *lineage) {
    if (strcmp(lineage, LAP_MAIN_LINEAGE) == 0 ||
        strcmp(lineage, h->lineage) == 0)
        return true;
    for (int32_t k = 0; k < h->n; k++) {
        if (strcmp(h->v[k].lineage, lineage) == 0)
            return true;
    }
    return false;
}

static int32_t branch_list(Arena *a, int32_t argc, char **argv, bool json) {
    static const char *const bool_flags[] = {"--json", NULL};
    if (!flags_known(argc, argv, NULL, bool_flags))
        return LAP_EXIT_ERR;
    Repo repo;
    char err[512];
    if (!repo_open(a, &repo, false, err, sizeof err)) {
        err_out(json, repo_error_code(), "%s", err);
        return LAP_EXIT_ERR;
    }
    RecLog log;
    if (!repo_log_load(a, &repo, &log, err, sizeof err)) {
        err_out(json, "log_unreadable", "%s", err);
        return LAP_EXIT_ERR;
    }
    Branches reg; /* with the branches started from these branches */
    branches_load_deep(a, repo.lapdir, &reg);
    const Hist *h = &repo.hist;
    /* Branches whose chunks are here though no registry lists them: brought
     * by git merge from another clone, or pruned once merged. Listed too,
     * from their branch records, so their sessions and the name of what
     * was adopted from them stay findable. */
    int32_t nreg = reg.n;
    const char **lins;
    int32_t nlins = hist_lineages(a, repo.lapdir, &lins);
    for (int32_t i = 0; i < nlins; i++) {
        Rec br;
        char ferr[256];
        if (in_this_history(h, lins[i]) || branches_find(&reg, lins[i]) ||
            !hist_first_record(a, repo.lapdir, lins[i], &br, ferr,
                               sizeof ferr) ||
            br.type != REC_BRANCH)
            continue;
        BranchEntry e = {lins[i], br.name, "", br.base, br.ts, NULL};
        if (br.parent && !in_this_history(h, br.parent))
            e.via = br.parent; /* a branch of another branch listed here */
        branches_add(a, &reg, e);
    }
    StrBuf sb;
    sb_init(&sb, a);
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
        const char *pname = h->parent; /* the branch it started from, named */
        for (int32_t i = 0; i < h->n; i++) {
            if (strcmp(h->v[i].lineage, h->parent) == 0)
                pname = hist_label(h, i);
        }
        sb_printf(&sb, "this folder is branch %s (%s) of %s, from %.7s\n",
                  h->name, h->lineage, pname, h->base);
    }
    for (int32_t i = 0; i < reg.n; i++) {
        const BranchEntry *e = &reg.v[i];
        /* A branch's merges are recorded in the folder that started it:
         * this one, or for a nested branch the branch that listed it. */
        const BranchEntry *via = e->via ? branches_find(&reg, e->via) : NULL;
        Repo vr;
        RecLog vlog;
        char verr[256];
        bool vopen = via && repo_open_at(a, &vr, via->path, false, verr,
                                         sizeof verr);
        BranchStatus st;
        if (vopen && repo_log_load(a, &vr, &vlog, verr, sizeof verr))
            branches_status(a, vr.lapdir, &vlog, e, &st);
        else
            branches_status(a, repo.lapdir, &log, e, &st);
        if (vopen)
            repo_close(&vr);
        /* no folder is known for it, so none is missing: work not merged
         * is going on elsewhere */
        bool registered = i < nreg;
        if (!registered && strcmp(st.state, "missing") == 0)
            st.state = "active";
        int32_t depth = 0; /* how many branches it is below this folder */
        for (const BranchEntry *p = via; p && depth < 32;
             p = p->via ? branches_find(&reg, p->via) : NULL)
            depth++;
        if (json) {
            sb_puts(&sb, i ? ",{\"id\":" : "{\"id\":");
            json_escape_c(&sb, e->id);
            sb_puts(&sb, ",\"name\":");
            json_escape_c(&sb, e->name);
            sb_printf(&sb, ",\"state\":\"%s\",\"present\":%s,\"path\":",
                      st.state, st.present ? "true" : "false");
            json_escape_c(&sb, e->path);
            sb_printf(&sb, ",\"registered\":%s", registered ? "true" : "false");
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
            sb_puts(&sb, "],\"stops\":[");
            for (int32_t k = 0; k < st.nstopped; k++) {
                sb_puts(&sb, k ? ",{\"file\":" : "{\"file\":");
                json_escape_c(&sb, st.stopped[k]);
                sb_printf(&sb, ",\"at\":\"%s\"}", st.stopped_at[k]);
            }
            if (e->via)
                sb_printf(&sb, "],\"via\":\"%s\"}", e->via);
            else
                sb_puts(&sb, "],\"via\":null}");
            continue;
        }
        int in = depth * 2; /* nested under the branch it started from */
        sb_printf(&sb, "%*s%-16s %-14s %s%s", in, "", e->name, st.state,
                  registered ? e->path : "(not registered here: its chunks "
                                         "are)",
                  st.present || !registered ? "" : " (gone)");
        if (via)
            sb_printf(&sb, "  (from %s)", via->name);
        sb_putc(&sb, '\n');
        if (st.readable)
            sb_printf(&sb, "%*s  %d commit%s since its base, %d since the "
                           "last merge\n",
                      in, "", st.since_base, st.since_base == 1 ? "" : "s",
                      st.since_merge);
        for (int32_t k = 0; k < st.nstopped; k++)
            sb_printf(&sb, "%*s  stopped: %s\n", in, "", st.stopped[k]);
        if (strcmp(st.state, "missing") == 0 && !via)
            sb_printf(&sb, "  its folder is gone: lap branch move %s <path> "
                           "if it moved, lap branch forget %s if it is no "
                           "more\n",
                      e->name, e->name);
        else if (strcmp(st.state, "missing") == 0)
            sb_printf(&sb, "%*s  its folder is gone: tend it from %s's "
                           "folder (lap branch move or forget there)\n",
                      in, "", via->name);
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
        err_out(json, repo_error_code(), "%s", err);
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
