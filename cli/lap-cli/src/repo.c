#include "repo.h"

#include "branches.h"
#include "json.h"
#include "sha256.h"
#include "snap.h"

/* Collapses ".", "..", doubled slashes; converts '\\' to '/'. In-place-safe
 * only with distinct buffers. Returns false on overflow or ".." escaping
 * the root of the path.
 */
static bool normalize_path(const char *in, char *out, size_t outsz) {
    /* Work on '/'-separated copy. */
    char buf[LAP_PATH_MAX];
    size_t n = strlen(in);
    if (n >= sizeof buf)
        return false;
    if (n == 0) {
        if (outsz < 2)
            return false;
        out[0] = '.';
        out[1] = '\0';
        return true;
    }
    memcpy(buf, in, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\\')
            buf[i] = '/';
    }

    /* Segment stack over out. */
    size_t o = 0;
    bool abs = buf[0] == '/';
#ifdef _WIN32
    /* keep a drive prefix like "C:" verbatim */
    const char *p = buf;
    if (n >= 2 && buf[1] == ':') {
        if (o + 2 >= outsz)
            return false;
        out[o++] = buf[0];
        out[o++] = buf[1];
        p = buf + 2;
        abs = *p == '/';
    }
#else
    const char *p = buf;
#endif
    if (abs) {
        if (o + 1 >= outsz)
            return false;
        out[o++] = '/';
        p++;
    }
    size_t seg_starts[512];
    int32_t nseg = 0;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
        if (seglen == 0 || (seglen == 1 && p[0] == '.')) {
            /* skip */
        } else if (seglen == 2 && p[0] == '.' && p[1] == '.') {
            if (nseg > 0) {
                o = seg_starts[--nseg];
            } else if (!abs) {
                return false; /* escapes the base */
            }
        } else {
            if (nseg >= 512)
                return false;
            seg_starts[nseg++] = o;
            if (o + seglen + 2 >= outsz)
                return false;
            memcpy(out + o, p, seglen);
            o += seglen;
            out[o++] = '/';
        }
        if (!slash)
            break;
        p = slash + 1;
    }
    /* strip trailing '/' unless the path is just the root */
    if (o > 1 && out[o - 1] == '/')
        o--;
#ifdef _WIN32
    if (o == 2 && out[1] == ':') {
        out[o++] = '/';
    }
#endif
    if (o == 0) {
        out[o++] = '.';
    }
    out[o] = '\0';
    return true;
}

static bool find_root(char *out, size_t outsz) {
    char cwd[LAP_PATH_MAX];
    if (!plat_getcwd(cwd, sizeof cwd))
        return false;
    char norm[LAP_PATH_MAX];
    if (!normalize_path(cwd, norm, sizeof norm))
        return false;
    for (;;) {
        char probe[LAP_PATH_MAX];
        if (snprintf(probe, sizeof probe, "%s/%s", norm, LAP_DIR) >=
            (int)sizeof probe)
            return false;
        if (plat_is_dir(probe)) {
            snprintf(out, outsz, "%s", norm);
            return true;
        }
        char *slash = strrchr(norm, '/');
        if (!slash || slash == norm)
            return false;
#ifdef _WIN32
        if (slash == norm + 2 && norm[1] == ':')
            return false;
#endif
        *slash = '\0';
    }
}

static void state_path(Repo *r, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", r->lapdir, LAP_STATE_NAME);
}

static bool state_write(Repo *r, char *err, size_t errsz) {
    StrBuf sb;
    sb_init(&sb, r->a);
    sb_puts(&sb, "{\"version\":1");
    sb_printf(&sb, ",\"next_commit\":%lld,\"next_session\":%lld",
              (long long)r->next_commit, (long long)r->next_session);
    if (r->active_session[0]) {
        sb_printf(&sb, ",\"active_session\":\"%s\"", r->active_session);
        sb_puts(&sb, ",\"active_session_msg\":");
        json_escape_c(&sb, r->active_session_msg);
    } else {
        sb_puts(&sb, ",\"active_session\":null");
    }
    if (r->cached_user[0]) {
        sb_puts(&sb, ",\"user_cache\":");
        json_escape_c(&sb, r->cached_user);
    }
    sb_printf(&sb, ",\"last_hash\":\"%s\"}", r->last_hash);
    size_t len = sb.len;
    char *data = sb_finish(&sb);
    char path[LAP_PATH_MAX];
    state_path(r, path, sizeof path);
    if (!plat_write_file_atomic(path, data, len)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    return true;
}

/* persist=true (writers, lock held): full cache rebuild via the index and
 * snapshot layer. persist=false (readers): counters recovered in memory by
 * a log scan — readers hold no lock and must not write anything.
 */
static bool state_heal(Repo *r, bool persist, char *err, size_t errsz) {
    if (persist) {
        if (!snap_rebuild_all(r->a, r, err, errsz))
            return false;
        return state_write(r, err, errsz);
    }
    /* Only the counters, the active session and the last hash are needed, so
     * each record is decoded into a scratch arena and dropped: healing costs
     * the log's bytes, not every record's text. rec_log_parse's rules hold:
     * a torn final line is ignored, and so are blank lines. */
    Arena *bytes = arena_new(1 << 16);
    char *data;
    size_t len;
    if (!hist_read_all(bytes, &r->hist, &data, &len)) {
        snprintf(err, errsz, "cannot read the history in %s", r->hist.dir);
        arena_free(bytes);
        return false;
    }
    while (len > 0 && data[len - 1] != '\n')
        len--;
    Arena *scratch = arena_new(1 << 16);
    int64_t next_c = 1, next_s = 1;
    r->active_session[0] = '\0';
    r->active_session_msg[0] = '\0';
    snprintf(r->last_hash, sizeof r->last_hash, "%s", LAP_HASH_ZERO);
    size_t start = 0;
    int32_t line_no = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n')
            continue;
        size_t n = i - start;
        const char *line = data + start;
        start = i + 1;
        line_no++;
        if (n == 0)
            continue;
        arena_reset(scratch);
        Rec one;
        char lerr[256];
        if (!rec_decode(scratch, line, n, &one, lerr, sizeof lerr)) {
            snprintf(err, errsz, "log line %d: %s", line_no, lerr);
            arena_free(scratch);
            arena_free(bytes);
            return false;
        }
        Rec *rec = &one;
        if (rec->type == REC_COMMIT && rec->id && rec->id[0] == 'L') {
            int64_t v = strtol(rec->id + 1, NULL, 10);
            if (v >= next_c)
                next_c = v + 1;
        } else if (rec->type == REC_SESSION_START && rec->id &&
                   rec->id[0] == 'S') {
            int64_t v = strtol(rec->id + 1, NULL, 10);
            if (v >= next_s)
                next_s = v + 1;
            if (!rec->from) { /* an adopted session is history, not open */
                snprintf(r->active_session, sizeof r->active_session, "%s",
                         rec->id);
                snprintf(r->active_session_msg,
                         sizeof r->active_session_msg, "%s",
                         rec->msg ? rec->msg : "");
            }
        } else if ((rec->type == REC_SESSION_END && !rec->from) ||
                   rec->type == REC_BRANCH) {
            /* a branch starts with no session open, whatever its parent
             * had open at the base */
            r->active_session[0] = '\0';
            r->active_session_msg[0] = '\0';
        }
        snprintf(r->last_hash, sizeof r->last_hash, "%s", rec->hash);
    }
    arena_free(scratch);
    arena_free(bytes);
    r->next_commit = next_c;
    r->next_session = next_s;
    return true;
}

bool repo_rebuild(Arena *a, Repo *r, char *err, size_t errsz) {
    (void)a;
    return state_heal(r, true, err, errsz);
}

const char *repo_user(Repo *r) {
    const char *env = getenv("LAP_USER");
    if (env && env[0])
        return env;
    if (r->cached_user[0])
        return r->cached_user;
#ifdef _WIN32
    FILE *p = _popen("git config --get user.name 2>NUL", "r");
#else
    FILE *p = popen("git config --get user.name 2>/dev/null", "r");
#endif
    char buf[128];
    buf[0] = '\0';
    if (p) {
        if (!fgets(buf, sizeof buf, p))
            buf[0] = '\0';
#ifdef _WIN32
        _pclose(p);
#else
        pclose(p);
#endif
    }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' '))
        buf[--n] = '\0';
    if (n == 0) {
        env = getenv("USER");
        if (!env || !env[0])
            env = getenv("USERNAME");
        snprintf(buf, sizeof buf, "%s", env && env[0] ? env : "unknown");
    }
    snprintf(r->cached_user, sizeof r->cached_user, "%s", buf);
    return r->cached_user;
}

bool repo_abspath(const char *user_path, char *out, size_t outsz) {
    char absbuf[LAP_PATH_MAX];
    bool is_abs = user_path[0] == '/';
#ifdef _WIN32
    if ((user_path[0] && user_path[1] == ':') || user_path[0] == '\\')
        is_abs = true;
#endif
    if (is_abs) {
        snprintf(absbuf, sizeof absbuf, "%s", user_path);
    } else {
        char cwd[LAP_PATH_MAX];
        if (!plat_getcwd(cwd, sizeof cwd) ||
            snprintf(absbuf, sizeof absbuf, "%s/%s", cwd, user_path) >=
                (int)sizeof absbuf)
            return false;
    }
    if (!normalize_path(absbuf, out, outsz))
        return false;
    /* an existing folder by its one real spelling, symlinks resolved, so
     * what lap stores and compares is the same however it was typed */
    char real[LAP_PATH_MAX];
    if (plat_realpath(out, real, sizeof real))
        snprintf(out, outsz, "%s", real);
    return true;
}

/* Writers drop the registry entries of branches merged up to their head
 * whose folder is gone — missing, or now holding another branch: their
 * history is in this folder's chunks, nothing is lost. Nothing else about
 * the registry is acted on, and nothing here can fail the command: hints
 * never do. */
static void prune_branches(Arena *a, Repo *r) {
    Branches reg;
    branches_load(a, r->lapdir, &reg);
    /* the common case costs one small read per branch */
    bool any_gone = false;
    for (int32_t i = 0; i < reg.n && !any_gone; i++) {
        char elap[LAP_PATH_MAX], lin[HIST_LINEAGE_MAX], err[256];
        snprintf(elap, sizeof elap, "%s/%s", reg.v[i].path, LAP_DIR);
        any_gone = !plat_is_dir(elap) ||
                   !hist_folder_lineage(a, elap, lin, err, sizeof err) ||
                   strcmp(lin, reg.v[i].id) != 0;
    }
    if (!any_gone)
        return;
    RecLog log;
    char err[256];
    if (!repo_log_load(a, r, &log, err, sizeof err))
        return;
    Branches keep;
    memset(&keep, 0, sizeof keep);
    for (int32_t i = 0; i < reg.n; i++) {
        BranchStatus st;
        branches_status(a, r->lapdir, &log, &reg.v[i], &st);
        if (!st.present && strcmp(st.state, "merged") == 0)
            continue;
        branches_add(a, &keep, reg.v[i]);
    }
    if (keep.n != reg.n)
        branches_save(a, r->lapdir, &keep);
}

/* Why the last repo_open failed, as an error code. */
static const char *open_code = "no_repo";

const char *repo_error_code(void) {
    return open_code;
}

/* A writer never builds on records it does not understand: what they mean
 * (a merge's, an amendment's) could make its own write wrong. Read only,
 * before anything is repaired or healed: the index's count, then the
 * history past what the index covers (all of it without an index). */
static bool newer_check(Arena *a, Repo *r, char *err, size_t errsz) {
    int32_t unknown = 0;
    const char *type = NULL;
    uint64_t from = 0; /* history bytes not yet looked at */
    IdxHeader h;
    if (idx_header(a, r, &h) && h.covered <= r->hist.size) {
        unknown = (int32_t)h.unknown;
        from = h.covered;
    }
    /* the bytes no index covers, a chunk at a time and each record in a
     * scratch arena: a history no index covers yet can be large */
    Arena *ca = arena_new(1 << 16), *ra = arena_new(1 << 16);
    bool counted = unknown > 0; /* the index already knows */
    for (int32_t k = 0; !counted && k < r->hist.n; k++) {
        uint64_t cend = r->hist.v[k].start + r->hist.v[k].size;
        if (cend <= from)
            continue;
        arena_reset(ca);
        char *data;
        size_t len = (size_t)(cend - from);
        if (!hist_read(ca, &r->hist, from, len, &data))
            break;
        for (size_t start = 0; start < len;) {
            const char *nl = memchr(data + start, '\n', len - start);
            if (!nl)
                break; /* a torn tail */
            size_t n = (size_t)(nl - (data + start));
            Rec rec;
            char ierr[128];
            arena_reset(ra);
            if (n > 0 &&
                rec_decode(ra, data + start, n, &rec, ierr, sizeof ierr) &&
                rec.type == REC_UNKNOWN && unknown++ == 0)
                type = arena_strdup(a, rec.name);
            start += n + 1;
        }
        from = cend;
    }
    arena_free(ca);
    arena_free(ra);
    if (unknown == 0)
        return true;
    open_code = "newer_history";
    snprintf(err, errsz,
             "this history holds %d record%s of a type this lap does not "
             "know%s%s%s: a newer lap wrote %s. Update lap before writing "
             "here (reading still works)",
             unknown, unknown == 1 ? "" : "s", type ? " (\"" : "",
             type ? type : "", type ? "\")" : "", unknown == 1 ? "it" : "them");
    return false;
}

bool repo_open(Arena *a, Repo *r, bool for_write, char *err, size_t errsz) {
    open_code = "no_repo";
    char root[LAP_PATH_MAX];
    if (!find_root(root, sizeof root)) {
        memset(r, 0, sizeof(*r));
        snprintf(err, errsz,
                 "not inside a lap repository (run \"lap init\" first)");
        return false;
    }
    return repo_open_at(a, r, root, for_write, err, errsz);
}

bool repo_open_at(Arena *a, Repo *r, const char *root, bool for_write,
                  char *err, size_t errsz) {
    memset(r, 0, sizeof(*r));
    r->a = a;
    snprintf(r->root, sizeof r->root, "%s", root);
    char probe[LAP_PATH_MAX];
    snprintf(probe, sizeof probe, "%s/%s", root, LAP_DIR);
    if (!plat_is_dir(probe)) {
        snprintf(err, errsz, "no lap repository at %s", root);
        return false;
    }
    snprintf(r->lapdir, sizeof r->lapdir, "%s/%s", r->root, LAP_DIR);

    if (for_write) {
        char lockpath[LAP_PATH_MAX];
        snprintf(lockpath, sizeof lockpath, "%s/%s", r->lapdir, LAP_LOCK_NAME);
        r->lock = plat_lock(a, lockpath);
        if (!r->lock) {
            snprintf(err, errsz, "cannot acquire repository lock");
            return false;
        }
        /* a folder from before chunks moves to them on its first write */
        bool converted;
        if (!hist_convert_legacy(a, r->lapdir, hist_chunk_limit(), &converted,
                                 err, errsz))
            return false;
        hist_clear_tmp(a, r->lapdir);
    }
    /* listed under the lock, so a writer's view cannot go stale */
    /* a repository is here; what follows is about its history, whose
     * damage (a missing chunk, a sealed one cut short) has its own code */
    open_code = "history_broken";
    if (!hist_open_folder(a, r->lapdir, &r->hist, err, errsz))
        return false;
    if (r->hist.n == 0) {
        snprintf(err, errsz, "no history in %s (expected %s/%s.000001.jsonl)",
                 r->hist.dir, LAP_LOG_DIR, LAP_MAIN_LINEAGE);
        return false;
    }
    /* Damage a reader names instead of tripping over (a sealed chunk cut
     * short), and a writer never builds on (also a chain broken between
     * chunks): refused before anything is written. */
    if (!hist_check(a, &r->hist, for_write, err, errsz))
        return false;
    open_code = "no_repo";
    if (for_write && !newer_check(a, r, err, errsz))
        return false;
    /* with the lock held, clean up any crash-torn append before we append
     * after it */
    if (for_write && !hist_repair_torn_tail(a, &r->hist)) {
        snprintf(err, errsz, "cannot repair torn log tail in %s",
                 r->hist.dir);
        return false;
    }

    char spath[LAP_PATH_MAX];
    state_path(r, spath, sizeof spath);
    char *sdata;
    size_t slen;
    bool healed = false;
    if (!plat_read_file_max(a, spath, &sdata, &slen, (size_t)-1)) {
        if (!state_heal(r, for_write, err, errsz))
            return false;
        healed = true;
    } else {
        char jerr[256];
        JVal *v = json_parse(a, sdata, slen, jerr, sizeof jerr);
        if (!v || v->t != J_OBJ) {
            if (!state_heal(r, for_write, err, errsz))
                return false;
            healed = true;
        } else {
            r->next_commit = jobj_int(v, "next_commit", 1);
            r->next_session = jobj_int(v, "next_session", 1);
            const char *as = jobj_str(v, "active_session");
            if (as)
                snprintf(r->active_session, sizeof r->active_session, "%s",
                         as);
            const char *asm_ = jobj_str(v, "active_session_msg");
            if (asm_)
                snprintf(r->active_session_msg, sizeof r->active_session_msg,
                         "%s", asm_);
            const char *uc = jobj_str(v, "user_cache");
            if (uc)
                snprintf(r->cached_user, sizeof r->cached_user, "%s", uc);
            const char *lh = jobj_str(v, "last_hash");
            snprintf(r->last_hash, sizeof r->last_hash, "%s",
                     lh ? lh : LAP_HASH_ZERO);
        }
    }

    if (!healed) {
        char tail[65];
        if (!hist_tail_hash(a, &r->hist, tail)) {
            snprintf(err, errsz, "cannot read the history in %s",
                     r->hist.dir);
            return false;
        }
        if (strcmp(tail, r->last_hash) != 0) {
            if (!state_heal(r, for_write, err, errsz))
                return false;
        }
    }
    if (for_write)
        prune_branches(a, r);
    return true;
}

void repo_close(Repo *r) {
    if (r->lock) {
        plat_unlock(r->lock);
        r->lock = NULL;
    }
}

bool repo_write_gitignore(const char *lapdir) {
    static const char text[] =
        "# Written by lap: only the history (log/) travels with the project.\n"
        "# Everything else here is this machine's: caches, the registry of\n"
        "# branches, and a branch folder's lineage and parent, which would\n"
        "# make any folder they reach through git think it is that branch.\n"
        "/*\n"
        "!/.gitignore\n"
        "!/log/\n";
    char path[LAP_PATH_MAX];
    snprintf(path, sizeof path, "%s/.gitignore", lapdir);
    if (plat_is_file(path))
        return true; /* the user's own, kept */
    return plat_write_file_atomic(path, text, sizeof text - 1);
}

bool repo_init(Arena *a, const char *dir, char *err, size_t errsz) {
    char lapdir[LAP_PATH_MAX];
    snprintf(lapdir, sizeof lapdir, "%s/%s", dir, LAP_DIR);
    if (plat_is_dir(lapdir)) {
        snprintf(err, errsz, "lap repository already exists at %s", lapdir);
        return false;
    }
    if (!plat_mkdirs(lapdir)) {
        snprintf(err, errsz, "cannot create %s", lapdir);
        return false;
    }
    char shadow[LAP_PATH_MAX];
    snprintf(shadow, sizeof shadow, "%s/%s", lapdir, LAP_SHADOW_NAME);
    if (!plat_mkdirs(shadow)) {
        snprintf(err, errsz, "cannot create %s", shadow);
        return false;
    }
    if (!repo_write_gitignore(lapdir)) {
        snprintf(err, errsz, "cannot write %s/.gitignore", lapdir);
        return false;
    }

    Repo r;
    memset(&r, 0, sizeof r);
    r.a = a;
    snprintf(r.root, sizeof r.root, "%s", dir);
    snprintf(r.lapdir, sizeof r.lapdir, "%s", lapdir);
    if (!hist_open(a, lapdir, LAP_MAIN_LINEAGE, &r.hist, err, errsz))
        return false;
    r.next_commit = 1;
    r.next_session = 1;
    snprintf(r.last_hash, sizeof r.last_hash, "%s", LAP_HASH_ZERO);

    Rec rec;
    memset(&rec, 0, sizeof rec);
    rec.type = REC_INIT;
    rec.version = 1;
    if (!repo_append(&r, &rec, err, errsz))
        return false;
    if (!repo_state_save(&r, err, errsz))
        return false;

    /* starter .lapignore (kept if the user already made one) */
    char igpath[LAP_PATH_MAX];
    snprintf(igpath, sizeof igpath, "%s/%s", dir, LAP_IGNORE_NAME);
    if (!plat_is_file(igpath)) {
        const char *ig =
            "# lap ignore patterns (gitignore-like subset; see lap help)\n"
            "# .lap/, .git/, .hg/, .svn/ and .DS_Store are always ignored.\n"
            "*.o\n"
            "*.tmp\n"
            "# the board's own history (coboard): not code\n"
            ".coboard/\n";
        plat_write_file_atomic(igpath, ig, strlen(ig));
    }
    return true;
}

bool repo_append(Repo *r, Rec *rec, char *err, size_t errsz) {
    if (!rec->ts) { /* adopted records keep the time the work was done */
        char ts[32];
        plat_timestamp(ts);
        rec->ts = arena_strdup(r->a, ts);
    }
    rec->prev = arena_strdup(r->a, r->last_hash);
    size_t len;
    char *line = rec_encode(r->a, rec, &len);
    char *with_nl = (char *)arena_alloc(r->a, len + 2);
    memcpy(with_nl, line, len);
    with_nl[len] = '\n';
    if (!hist_append(r->a, &r->hist, with_nl, len + 1, err, errsz))
        return false;
    snprintf(r->last_hash, sizeof r->last_hash, "%s", rec->hash);
    return true;
}

static void where_in_history(const void *ctx, const char *data, uint64_t off,
                             char *out, size_t outsz) {
    hist_where((const Hist *)ctx, data, off, out, outsz);
}

bool repo_log_load(Arena *a, Repo *r, RecLog *out, char *err, size_t errsz) {
    char *data;
    size_t len;
    /* lap's own history must never become unreadable by growing: no cap */
    if (!hist_read_all(a, &r->hist, &data, &len)) {
        snprintf(err, errsz, "cannot read the history in %s", r->hist.dir);
        return false;
    }
    if (!rec_log_parse(a, data, len, where_in_history, &r->hist, out, err,
                       errsz))
        return false;
    if (out->unknown_n > 0)
        rec_note_newer(out->unknown_type);
    for (int32_t i = 0, k = 0; i < out->count; i++) {
        uint64_t off = (uint64_t)(out->v[i].raw - data);
        while (k + 1 < r->hist.n && r->hist.v[k + 1].start <= off)
            k++; /* records are in chunk order */
        out->v[i].lineage = hist_label(&r->hist, k);
    }
    rec_amend_log(a, out);
    if (!out->chain_ok)
        hist_name_break(&r->hist, data, out);
    return true;
}

bool repo_state_save(Repo *r, char *err, size_t errsz) {
    return state_write(r, err, errsz);
}

bool repo_relpath(Repo *r, const char *user_path, char *out, size_t outsz,
                  char *err, size_t errsz) {
    char absbuf[LAP_PATH_MAX];
    bool is_abs = user_path[0] == '/';
#ifdef _WIN32
    if ((user_path[0] && user_path[1] == ':') || user_path[0] == '\\')
        is_abs = true;
#endif
    if (is_abs) {
        snprintf(absbuf, sizeof absbuf, "%s", user_path);
    } else {
        char cwd[LAP_PATH_MAX];
        if (!plat_getcwd(cwd, sizeof cwd)) {
            snprintf(err, errsz, "cannot get working directory");
            return false;
        }
        if (snprintf(absbuf, sizeof absbuf, "%s/%s", cwd, user_path) >=
            (int)sizeof absbuf) {
            snprintf(err, errsz, "path too long");
            return false;
        }
    }
    char norm[LAP_PATH_MAX];
    if (!normalize_path(absbuf, norm, sizeof norm)) {
        snprintf(err, errsz, "cannot normalize path %s", user_path);
        return false;
    }
    size_t rootlen = strlen(r->root);
    if (strncmp(norm, r->root, rootlen) != 0 ||
        (norm[rootlen] != '/' && norm[rootlen] != '\0')) {
        snprintf(err, errsz, "path %s is outside the repository (%s)",
                 user_path, r->root);
        return false;
    }
    if (norm[rootlen] == '\0') {
        snprintf(err, errsz, "path %s is the repository root, not a file",
                 user_path);
        return false;
    }
    snprintf(out, outsz, "%s", norm + rootlen + 1);
    return true;
}

static void shadow_path(Repo *r, const char *rel, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s/%s", r->lapdir, LAP_SHADOW_NAME, rel);
}

bool shadow_read(Arena *a, Repo *r, const char *rel, char **data,
                 size_t *len, bool *exists) {
    char path[LAP_PATH_MAX];
    shadow_path(r, rel, path, sizeof path);
    if (!plat_is_file(path)) {
        *exists = false;
        *data = NULL;
        *len = 0;
        return true;
    }
    *exists = true;
    return plat_read_file(a, path, data, len);
}

bool shadow_write(Repo *r, const char *rel, const void *data, size_t len) {
    char path[LAP_PATH_MAX];
    shadow_path(r, rel, path, sizeof path);
    /* ensure parent dirs */
    char parent[LAP_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (!plat_mkdirs(parent))
            return false;
    }
    return plat_write_file_atomic(path, data, len);
}

bool shadow_remove(Repo *r, const char *rel) {
    char path[LAP_PATH_MAX];
    shadow_path(r, rel, path, sizeof path);
    return plat_remove_file(path);
}

bool looks_binary(const char *data, size_t len) {
    size_t check = len < 8192 ? len : 8192;
    return memchr(data, '\0', check) != NULL;
}
