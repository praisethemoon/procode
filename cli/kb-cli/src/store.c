#include "store.h"

#include "errdet.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

/* Collapses ".", "..", doubled slashes; converts '\\' to '/'. Adapted from
 * lap's repo.c, which normalizes paths for the same reason: the walk up to
 * the filesystem root is a string operation, and it is only correct on a
 * path with no "." or ".." left in it.
 */
static bool normalize_path(const char *in, char *out, size_t outsz) {
    char buf[KB_PATH_MAX];
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

    size_t o = 0;
    bool abs = buf[0] == '/';
#ifdef _WIN32
    const char *p = buf;
    if (n >= 2 && buf[1] == ':') { /* keep a drive prefix like "C:" */
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
    if (o > 1 && out[o - 1] == '/')
        o--;
#ifdef _WIN32
    if (o == 2 && out[1] == ':')
        out[o++] = '/';
#endif
    if (o == 0)
        out[o++] = '.';
    out[o] = '\0';
    return true;
}

/* True once `dir` is the topmost directory the walk can reach: "/" on
 * POSIX, "C:/" on Windows. */
static bool at_filesystem_root(const char *dir) {
    if (dir[0] == '/' && dir[1] == '\0')
        return true;
#ifdef _WIN32
    if (dir[0] && dir[1] == ':' && (dir[2] == '\0' || dir[2] == '/'))
        return dir[2] == '\0' || dir[3] == '\0';
#endif
    return false;
}

/* The directory the home directory resolves to, or false when there is no
 * home. `HOME` may be a symbolic link to where the working directory really
 * is, so both sides are compared canonically. */
static bool home_dir(char *out, size_t outsz) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0])
        home = getenv("USERPROFILE");
#endif
    return home && home[0] && plat_realpath(home, out, outsz);
}

/* Whether `probe`, a directory named `.kb` in `parent`, is a store.
 *
 * ONLY WHAT `kb init` MADE. `store_create` writes the documents log from the
 * start, so every real store has one; a `.kb` directory without it — the
 * home directory's, which holds only `models/`, or any stray one — is walked
 * past like a plain file named `.kb` is.
 *
 * AND NEVER THE HOME DIRECTORY'S. `~/.kb` is the machine's models (§8), and
 * §1.4 has no store in the home directory: were it a store, every folder
 * under the home directory without one of its own would read and write
 * there. Refused even when it holds logs, so that a store a buggy version
 * wrote there is not adopted either. */
bool store_is_store(const char *parent, const char *probe) {
    char home[KB_PATH_MAX];
    char here[KB_PATH_MAX];
    if (home_dir(home, sizeof home) && plat_realpath(parent, here, sizeof here) && strcmp(home, here) == 0)
        return false;
    char log[KB_PATH_MAX];
    if (snprintf(log, sizeof log, "%s/%s", probe, KB_DOCUMENTS_NAME) >= (int)sizeof log)
        return false;
    return plat_is_dir(probe) && plat_is_file(log);
}

/* Whether `dir` is the home directory, where §1.4 allows no store. */
bool store_is_home(const char *dir) {
    char home[KB_PATH_MAX];
    char here[KB_PATH_MAX];
    return home_dir(home, sizeof home) && plat_realpath(dir, here, sizeof here) && strcmp(home, here) == 0;
}

bool store_find(char *out, size_t outsz) {
    char cwd[KB_PATH_MAX];
    if (!plat_getcwd(cwd, sizeof cwd))
        return false;
    char dir[KB_PATH_MAX];
    if (!normalize_path(cwd, dir, sizeof dir))
        return false;
    for (;;) {
        char probe[KB_PATH_MAX];
        /* "/" already ends in a separator; anything else needs one. */
        int32_t need = snprintf(probe, sizeof probe, "%s%s%s", dir,
                                at_filesystem_root(dir) ? "" : "/", KB_DIR);
        if (need >= (int32_t)sizeof probe)
            return false;
        /* A plain file named .kb is not a store, nor is a directory that
         * `kb init` did not make, nor the home directory's (store_is_store).
         * Walking on past them is the point: nothing stray may shadow a real
         * store further up. */
        if (store_is_store(dir, probe)) {
            snprintf(out, outsz, "%s", probe);
            return true;
        }
        if (at_filesystem_root(dir))
            return false; /* probed the root; there is nowhere above it */
        char *slash = strrchr(dir, '/');
        if (!slash)
            return false;
        /* "/a" -> "/", not "" */
        if (slash == dir)
            dir[1] = '\0';
#ifdef _WIN32
        else if (slash == dir + 2 && dir[1] == ':')
            dir[3] = '\0'; /* "C:/a" -> "C:/", not "C:a" */
#endif
        else
            *slash = '\0';
    }
}

bool store_abs_path(const char *in, char *out, size_t outsz) {
    bool is_abs = in[0] == '/';
#ifdef _WIN32
    if ((in[0] && in[1] == ':') || in[0] == '\\')
        is_abs = true;
#endif
    char joined[KB_PATH_MAX];
    if (is_abs) {
        if (snprintf(joined, sizeof joined, "%s", in) >= (int32_t)sizeof joined)
            return false;
    } else {
        char cwd[KB_PATH_MAX];
        if (!plat_getcwd(cwd, sizeof cwd))
            return false;
        if (snprintf(joined, sizeof joined, "%s/%s", cwd, in) >=
            (int32_t)sizeof joined)
            return false;
    }
    return normalize_path(joined, out, outsz);
}

static void store_paths(Store *s, const char *dir) {
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    snprintf(s->sources_path, sizeof s->sources_path, "%s/%s", dir,
             KB_SOURCES_NAME);
    snprintf(s->documents_path, sizeof s->documents_path, "%s/%s", dir,
             KB_DOCUMENTS_NAME);
    snprintf(s->blobs_dir, sizeof s->blobs_dir, "%s/%s", dir, KB_BLOBS_NAME);
    snprintf(s->index_dir, sizeof s->index_dir, "%s/%s", dir, KB_INDEX_NAME);
}

bool store_create(Arena *a, const char *dir, char *err, size_t errsz) {
    (void)a;
    if (plat_is_dir(dir)) {
        snprintf(err, errsz, "a kb store already exists at %s", dir);
        return false;
    }
    char sub[KB_PATH_MAX];
    if (!plat_mkdirs(dir)) {
        snprintf(err, errsz, "cannot create %s", dir);
        return false;
    }
    snprintf(sub, sizeof sub, "%s/%s", dir, KB_BLOBS_NAME);
    if (!plat_mkdirs(sub)) {
        snprintf(err, errsz, "cannot create %s", sub);
        return false;
    }
    snprintf(sub, sizeof sub, "%s/%s", dir, KB_INDEX_NAME);
    if (!plat_mkdirs(sub)) {
        snprintf(err, errsz, "cannot create %s", sub);
        return false;
    }
    /* The logs exist from the start so that a fresh store looks the same to
     * a reader as one that has been emptied. */
    snprintf(sub, sizeof sub, "%s/%s", dir, KB_SOURCES_NAME);
    if (!plat_write_file_atomic(sub, "", 0)) {
        snprintf(err, errsz, "cannot create %s", sub);
        return false;
    }
    snprintf(sub, sizeof sub, "%s/%s", dir, KB_DOCUMENTS_NAME);
    if (!plat_write_file_atomic(sub, "", 0)) {
        snprintf(err, errsz, "cannot create %s", sub);
        return false;
    }
    /* index/ and nothing else: the logs and the blobs are the truth and are
     * worth sharing, so a collaborator who clones gets the whole corpus and
     * rebuilds the caches locally (§1.5). */
    snprintf(sub, sizeof sub, "%s/%s", dir, KB_GITIGNORE_NAME);
    static const char gi[] = KB_INDEX_NAME "/\n";
    if (!plat_write_file_atomic(sub, gi, sizeof gi - 1)) {
        snprintf(err, errsz, "cannot create %s", sub);
        return false;
    }
    return true;
}

/* ---- id counters ------------------------------------------------------ */

static void counters_path(const Store *s, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", s->dir, KB_COUNTERS_NAME);
}

static bool counters_write(Store *s, char *err, size_t errsz) {
    char path[KB_PATH_MAX];
    counters_path(s, path, sizeof path);
    char buf[256];
    int32_t n = snprintf(buf, sizeof buf,
                         "{\"version\":1,\"next_source\":%lld,"
                         "\"next_document\":%lld,\"next_chunk\":%lld}\n",
                         (long long)s->next_source, (long long)s->next_document,
                         (long long)s->next_chunk);
    if (!plat_write_file_atomic(path, buf, (size_t)n)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    return true;
}

/* The counters are a cache; the logs are the truth. So they are read for
 * their value and then floored by what the logs actually contain, and the
 * floor comes from the highest id ever WRITTEN — including records later
 * superseded, and including a tombstone left behind by a delete, because
 * the floor is computed over raw records rather than over the fold. An id
 * that is gone from the store is still an id that was handed out. */
static void counters_load(Store *s) {
    char path[KB_PATH_MAX];
    counters_path(s, path, sizeof path);
    s->next_source = s->next_document = s->next_chunk = 1;
    char *data;
    size_t len;
    if (plat_read_file_max(s->a, path, &data, &len, (size_t)-1)) {
        char jerr[256];
        JVal *v = json_parse(s->a, data, len, jerr, sizeof jerr);
        if (v && v->t == J_OBJ) {
            s->next_source = jobj_int(v, "next_source", 1);
            s->next_document = jobj_int(v, "next_document", 1);
            s->next_chunk = jobj_int(v, "next_chunk", 1);
        }
    }
    if (s->next_source < s->sources.max_id + 1)
        s->next_source = s->sources.max_id + 1;
    if (s->next_document < s->documents.max_id + 1)
        s->next_document = s->documents.max_id + 1;
    if (s->next_chunk < s->documents.max_chunk_id + 1)
        s->next_chunk = s->documents.max_chunk_id + 1;
}

bool store_reserve(Store *s, uint32_t nsources, uint32_t ndocuments,
                   uint32_t nchunks, int64_t *src, int64_t *doc,
                   int64_t *chunk_base, char *err, size_t errsz) {
    if (!s->lock) {
        snprintf(err, errsz, "internal: id reservation without the lock");
        return false;
    }
    if (nsources) {
        *src = s->next_source;
        s->next_source += nsources;
    }
    if (ndocuments) {
        *doc = s->next_document;
        s->next_document += ndocuments;
    }
    if (nchunks) {
        *chunk_base = s->next_chunk;
        s->next_chunk += nchunks;
    }
    /* Durable before the caller may use them. A crash after this point has
     * already spent these ids; a crash before it spent nothing. There is no
     * window in which the same id can come out twice. */
    return counters_write(s, err, errsz);
}

/* ---- torn-tail repair ------------------------------------------------- */

/* Truncates an unterminated final line. Reads the tail in a growing window
 * rather than the whole log: repair runs before every write, and a log that
 * has to be read in full to be appended to would get slower forever. */
static bool repair_torn_tail(Arena *a, const char *path, char *err,
                             size_t errsz) {
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
            window *= 2; /* the torn line alone exceeds the window */
            continue;
        }
        uint64_t keep = fsize - (uint64_t)(len - end);
        if (!plat_truncate(path, keep)) {
            snprintf(err, errsz, "cannot repair torn tail of %s", path);
            return false;
        }
        fprintf(stderr,
                "kb: dropped %llu bytes of an interrupted append to %s\n",
                (unsigned long long)(fsize - keep), path);
        return true;
    }
}

/* ---- open / close ----------------------------------------------------- */

bool store_open(Arena *a, Store *s, const char *dir, bool for_write, char *err,
                size_t errsz, const char **code) {
    memset(s, 0, sizeof(*s));
    s->a = a;
    *code = "internal";
    if (!plat_is_dir(dir)) {
        snprintf(err, errsz, "no kb store at %s", dir);
        *code = "not_found";
        return false;
    }
    store_paths(s, dir);

    if (for_write) {
        /* index/ is derived and disposable, so a clone that never ran
         * rebuild does not have one. Creating it is not a mutation of the
         * store's truth, and the lock has to live somewhere. */
        if (!plat_mkdirs(s->index_dir)) {
            snprintf(err, errsz, "cannot create %s", s->index_dir);
            return false;
        }
        char lockpath[KB_PATH_MAX];
        snprintf(lockpath, sizeof lockpath, "%s/%s", dir, KB_LOCK_NAME);
        int64_t holder = 0;
        s->lock = plat_lock_try(a, lockpath, &holder);
        if (!s->lock) {
            if (holder < 0) {
                snprintf(err, errsz, "cannot open lock file %s", lockpath);
                return false;
            }
            if (holder > 0)
                snprintf(err, errsz,
                         "store %s is locked by process %lld", dir,
                         (long long)holder);
            else
                snprintf(err, errsz, "store %s is locked by another process",
                         dir);
            *code = "store_locked";
            errdet_begin("store_locked");
            errdet_str("store", dir);
            if (holder > 0)
                errdet_int("pid", holder);
            return false;
        }
        /* Before anything is read and long before anything is appended: a
         * fragment left by a crash must not be glued to the next record. */
        if (!repair_torn_tail(a, s->sources_path, err, errsz) ||
            !repair_torn_tail(a, s->documents_path, err, errsz)) {
            store_close(s);
            return false;
        }
    }

    if (!srclog_load(a, s->sources_path, &s->sources, err, errsz) ||
        !doclog_load(a, s->documents_path, &s->documents, err, errsz)) {
        *code = "corrupt_log";
        store_close(s);
        return false;
    }
    counters_load(s);
    return true;
}

void store_close(Store *s) {
    if (s->lock) {
        plat_unlock(s->lock);
        s->lock = NULL;
    }
}

/* ---- appends ---------------------------------------------------------- */

bool store_append(Store *s, StoreLogId which, const char *line, size_t len,
                  char *err, size_t errsz) {
    if (!s->lock) {
        snprintf(err, errsz, "internal: append without the store lock");
        return false;
    }
    const char *path =
        which == STORE_SOURCES ? s->sources_path : s->documents_path;
    /* One write, terminator included. Two writes would leave a window in
     * which the record is on disk without its frame. */
    char *buf = (char *)arena_alloc(s->a, len + 2);
    memcpy(buf, line, len);
    buf[len] = '\n';
    if (!plat_append_file_sync(path, buf, len + 1)) {
        snprintf(err, errsz, "cannot append to %s", path);
        return false;
    }
    return true;
}

/* ---- blobs ------------------------------------------------------------ */

void store_blob_path(const Store *s, const char *hash, char *out,
                     size_t outsz) {
    snprintf(out, outsz, "%s/%s", s->blobs_dir, hash);
}

bool store_put_blob(Store *s, const void *data, size_t len, char hash[65],
                    bool *written, char *err, size_t errsz) {
    sha256_hex(data, len, hash);
    char path[KB_PATH_MAX];
    store_blob_path(s, hash, path, sizeof path);
    *written = false;
    /* Content-addressed: a blob that is already there has, by construction,
     * the content we were about to write. Re-ingesting unchanged text
     * writes nothing (§1.6). */
    if (plat_is_file(path))
        return true;
    if (!plat_mkdirs(s->blobs_dir)) {
        snprintf(err, errsz, "cannot create %s", s->blobs_dir);
        return false;
    }
    if (!plat_write_file_atomic(path, data, len)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    *written = true;
    return true;
}

/* The hash comes out of a log line or a directory listing, both of which are
 * files a person can edit. A blob name is 64 lowercase hex digits and nothing
 * else, so anything that is not one is refused rather than joined onto a path
 * — and "../../etc/passwd" is refused by the same rule that refuses "CAFE".
 * One predicate, so the reader and the deleter cannot disagree about what a
 * blob name is. */
bool store_is_blob_name(const char *hash) {
    if (!hash || strlen(hash) != 64)
        return false;
    for (const char *p = hash; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return false;
    }
    return true;
}

bool store_get_blob(Store *s, const char *hash, char **data, size_t *len) {
    if (!store_is_blob_name(hash))
        return false;
    char path[KB_PATH_MAX];
    store_blob_path(s, hash, path, sizeof path);
    return plat_read_file_max(s->a, path, data, len, (size_t)-1);
}

bool store_drop_blob(Store *s, const char *hash) {
    if (!s->lock || !store_is_blob_name(hash))
        return false;
    char path[KB_PATH_MAX];
    store_blob_path(s, hash, path, sizeof path);
    return plat_remove_file(path);
}

/* ---- model.json ------------------------------------------------------- */

ChunkParams store_chunk_params(Arena *a, const Store *s) {
    ChunkParams p;
    p.chunker = KB_CHUNKER_ID;
    p.chunk_tokens = KB_CHUNK_TOKENS;
    p.chunk_overlap = KB_CHUNK_OVERLAP;
    p.present = false;
    char path[KB_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s->dir, KB_MODEL_NAME);
    char *data;
    size_t len;
    if (!plat_read_file_max(a, path, &data, &len, (size_t)-1))
        return p;
    char jerr[256];
    JVal *v = json_parse(a, data, len, jerr, sizeof jerr);
    if (!v || v->t != J_OBJ)
        return p;
    p.present = true;
    const char *c = jobj_str(v, "chunker");
    if (c)
        p.chunker = c;
    p.chunk_tokens = (uint32_t)jobj_int(v, "chunkTokens", KB_CHUNK_TOKENS);
    p.chunk_overlap = (uint32_t)jobj_int(v, "chunkOverlap", KB_CHUNK_OVERLAP);
    return p;
}

static bool put_chunk_params(Store *s, char *err, size_t errsz) {
    char path[KB_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s->dir, KB_MODEL_NAME);
    if (!plat_mkdirs(s->index_dir)) {
        snprintf(err, errsz, "cannot create %s", s->index_dir);
        return false;
    }
    char buf[256];
    int32_t n = snprintf(buf, sizeof buf,
                         "{\"chunker\":\"%s\",\"chunkTokens\":%lu,"
                         "\"chunkOverlap\":%lu}\n",
                         KB_CHUNKER_ID, (unsigned long)KB_CHUNK_TOKENS,
                         (unsigned long)KB_CHUNK_OVERLAP);
    if (!plat_write_file_atomic(path, buf, (size_t)n)) {
        snprintf(err, errsz, "cannot write %s", path);
        return false;
    }
    return true;
}

bool store_write_chunk_params(Store *s, char *err, size_t errsz) {
    char path[KB_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", s->dir, KB_MODEL_NAME);
    /* Never overwrite: this file states what the index on disk was actually
     * built with. Rewriting it to match the current build would erase the
     * only evidence that a reindex is owed (§8). */
    if (plat_is_file(path))
        return true;
    return put_chunk_params(s, err, errsz);
}

bool store_rewrite_chunk_params(Store *s, char *err, size_t errsz) {
    /* Written under the lock, like every other change to a store's truth —
     * and after the re-chunked records are already appended, so a crash in
     * between leaves the file saying the OLD parameters while the log holds
     * the NEW ranges. That state is detected (a re-split under the recorded
     * parameters no longer matches the log) and a second reindex converges
     * on it. The other order would leave a state a second reindex reads as
     * already correct. */
    if (!s->lock) {
        snprintf(err, errsz, "internal: model.json rewritten without the lock");
        return false;
    }
    return put_chunk_params(s, err, errsz);
}

/* ---- disk use --------------------------------------------------------- */

typedef struct {
    const Store *s;
    uint64_t total;
    uint64_t index;
} DiskWalk;

static WalkAction disk_visit(const char *rel, bool is_dir, void *ud) {
    DiskWalk *w = (DiskWalk *)ud;
    if (is_dir)
        return WALK_CONT;
    char path[KB_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", w->s->dir, rel);
    uint64_t sz = 0;
    if (!plat_file_size(path, &sz))
        return WALK_CONT;
    w->total += sz;
    if (strncmp(rel, KB_INDEX_NAME "/", sizeof KB_INDEX_NAME) == 0)
        w->index += sz;
    return WALK_CONT;
}

uint64_t store_disk_bytes(Arena *a, const Store *s, uint64_t *index_bytes) {
    DiskWalk w = {s, 0, 0};
    plat_walk(a, s->dir, disk_visit, &w);
    if (index_bytes)
        *index_bytes = w.index;
    return w.total;
}
