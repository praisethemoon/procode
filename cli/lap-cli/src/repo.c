#include "repo.h"

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
     * the log's bytes, not every record's text. rec_log_load's rules hold:
     * a torn final line is ignored, and so are blank lines. */
    Arena *bytes = arena_new(1 << 16);
    char *data;
    size_t len;
    if (!plat_read_file_max(bytes, r->logpath, &data, &len, (size_t)-1)) {
        snprintf(err, errsz, "cannot read log file %s", r->logpath);
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
            snprintf(r->active_session, sizeof r->active_session, "%s",
                     rec->id);
            snprintf(r->active_session_msg, sizeof r->active_session_msg, "%s",
                     rec->msg ? rec->msg : "");
        } else if (rec->type == REC_SESSION_END) {
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

/* Reads the last complete line's hash from the log using a bounded tail
 * window (doubling until the line fits) — never the whole file, and never
 * subject to any size cap. A torn unterminated tail is skipped.
 */
static bool log_tail_hash(Arena *a, const char *path, char out[65]) {
    size_t window = 64 * 1024;
    for (;;) {
        char *data;
        size_t len;
        uint64_t fsize;
        if (!plat_read_tail(a, path, window, &data, &len, &fsize))
            return false;
        if (len == 0) {
            snprintf(out, 65, "%s", LAP_HASH_ZERO);
            return true;
        }
        bool whole_file = (uint64_t)len == fsize;
        /* skip a torn (unterminated) tail */
        size_t end = len;
        while (end > 0 && data[end - 1] != '\n')
            end--;
        if (end == 0) {
            if (whole_file) {
                snprintf(out, 65, "%s", LAP_HASH_ZERO);
                return true;
            }
            window *= 2;
            continue; /* the torn line alone exceeds the window */
        }
        /* end points just past the '\n' of the last complete line; skip
         * any blank lines above it */
        size_t line_end = end - 1;
        while (line_end > 0 && data[line_end - 1] == '\n')
            line_end--;
        if (line_end == 0) {
            if (whole_file) {
                snprintf(out, 65, "%s", LAP_HASH_ZERO);
                return true;
            }
            window *= 2;
            continue;
        }
        size_t start = line_end;
        while (start > 0 && data[start - 1] != '\n')
            start--;
        if (start == 0 && !whole_file) {
            window *= 2; /* line may begin before the window */
            continue;
        }
        sha256_hex(data + start, line_end - start, out);
        return true;
    }
}

/* A crash mid-append leaves an unterminated final line. Writers repair the
 * log by truncating it away (the record was never acknowledged: state.json
 * was not updated and the appender reported failure). Lock must be held.
 */
static bool log_repair_torn_tail(Arena *a, const char *path) {
    size_t window = 64 * 1024;
    for (;;) {
        char *data;
        size_t len;
        uint64_t fsize;
        if (!plat_read_tail(a, path, window, &data, &len, &fsize))
            return true; /* no log yet: nothing to repair */
        if (len == 0 || data[len - 1] == '\n')
            return true;
        size_t end = len;
        while (end > 0 && data[end - 1] != '\n')
            end--;
        if (end == 0 && (uint64_t)len != fsize) {
            window *= 2;
            continue;
        }
        uint64_t keep = fsize - (uint64_t)(len - end);
        uint64_t dropped = fsize - keep;
        if (!plat_truncate(path, keep))
            return false;
        fprintf(stderr,
                "lap: repaired torn log tail (%llu bytes from an "
                "interrupted append dropped)\n",
                (unsigned long long)dropped);
        return true;
    }
}

bool repo_open(Arena *a, Repo *r, bool for_write, char *err, size_t errsz) {
    memset(r, 0, sizeof(*r));
    r->a = a;
    if (!find_root(r->root, sizeof r->root)) {
        snprintf(err, errsz,
                 "not inside a lap repository (run \"lap init\" first)");
        return false;
    }
    snprintf(r->lapdir, sizeof r->lapdir, "%s/%s", r->root, LAP_DIR);
    snprintf(r->logpath, sizeof r->logpath, "%s/%s", r->lapdir, LAP_LOG_NAME);

    if (for_write) {
        char lockpath[LAP_PATH_MAX];
        snprintf(lockpath, sizeof lockpath, "%s/%s", r->lapdir, LAP_LOCK_NAME);
        r->lock = plat_lock(a, lockpath);
        if (!r->lock) {
            snprintf(err, errsz, "cannot acquire repository lock");
            return false;
        }
        /* with the lock held, clean up any crash-torn append before we
         * append after it */
        if (!log_repair_torn_tail(a, r->logpath)) {
            snprintf(err, errsz, "cannot repair torn log tail in %s",
                     r->logpath);
            return false;
        }
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
        if (!log_tail_hash(a, r->logpath, tail)) {
            snprintf(err, errsz, "cannot read log %s", r->logpath);
            return false;
        }
        if (strcmp(tail, r->last_hash) != 0) {
            if (!state_heal(r, for_write, err, errsz))
                return false;
        }
    }
    return true;
}

void repo_close(Repo *r) {
    if (r->lock) {
        plat_unlock(r->lock);
        r->lock = NULL;
    }
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

    Repo r;
    memset(&r, 0, sizeof r);
    r.a = a;
    snprintf(r.root, sizeof r.root, "%s", dir);
    snprintf(r.lapdir, sizeof r.lapdir, "%s", lapdir);
    snprintf(r.logpath, sizeof r.logpath, "%s/%s", lapdir, LAP_LOG_NAME);
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
            "*.tmp\n";
        plat_write_file_atomic(igpath, ig, strlen(ig));
    }
    return true;
}

bool repo_append(Repo *r, Rec *rec, char *err, size_t errsz) {
    char ts[32];
    plat_timestamp(ts);
    rec->ts = arena_strdup(r->a, ts);
    rec->prev = arena_strdup(r->a, r->last_hash);
    size_t len;
    char *line = rec_encode(r->a, rec, &len);
    char *with_nl = (char *)arena_alloc(r->a, len + 2);
    memcpy(with_nl, line, len);
    with_nl[len] = '\n';
    if (!plat_append_file_sync(r->logpath, with_nl, len + 1)) {
        snprintf(err, errsz, "cannot append to %s", r->logpath);
        return false;
    }
    snprintf(r->last_hash, sizeof r->last_hash, "%s", rec->hash);
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
