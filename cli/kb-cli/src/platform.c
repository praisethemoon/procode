#include "platform.h"

#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING /* absent in older SDK headers */
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

/* Converts '/' to '\\' into a stack buffer for Win32 calls. */
static const char *winpath(char *buf, size_t bufsz, const char *path) {
    size_t n = strlen(path);
    if (n >= bufsz)
        n = bufsz - 1;
    for (size_t i = 0; i < n; i++)
        buf[i] = path[i] == '/' ? '\\' : path[i];
    buf[n] = '\0';
    return buf;
}
#endif

bool plat_is_file(const char *path) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    DWORD attr = GetFileAttributesA(winpath(wb, sizeof wb, path));
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool plat_is_dir(const char *path) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    DWORD attr = GetFileAttributesA(winpath(wb, sizeof wb, path));
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* "Does this already exist as a directory?" — and that question must follow
 * symlinks, which is why it cannot reuse `plat_is_dir`.
 *
 * `plat_is_dir` is deliberately `lstat`-based: a `.kb` that is itself a
 * symlink is not a store, and treating it as one would let a link decide where
 * the logs live. But `mkdir -p` asks a different question, and asking it with
 * `lstat` made every directory under a symlinked ancestor fail to be created —
 * on macOS both `/tmp` and `/var` are symlinks, so anything under `$TMPDIR`
 * hit it, which is most of the test suite.
 *
 * lap-cli has the same bug and does not have this fix. */
static bool exists_as_dir(const char *path) {
#ifdef _WIN32
    return plat_is_dir(path);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool plat_mkdir(const char *path) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    if (_mkdir(winpath(wb, sizeof wb, path)) == 0)
        return true;
    return plat_is_dir(path);
#else
    if (mkdir(path, 0777) == 0)
        return true;
    return errno == EEXIST && exists_as_dir(path);
#endif
}

bool plat_mkdirs(const char *path) {
    char buf[KB_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof buf)
        return false;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
#ifdef _WIN32
            /* skip drive roots like "C:" */
            if (!(i == 2 && buf[1] == ':'))
                if (!plat_mkdir(buf))
                    return false;
#else
            if (!plat_mkdir(buf))
                return false;
#endif
            buf[i] = '/';
        }
    }
    return plat_mkdir(buf);
}

bool plat_remove_file(const char *path) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    return DeleteFileA(winpath(wb, sizeof wb, path)) != 0;
#else
    return unlink(path) == 0;
#endif
}

bool plat_getcwd(char *buf, size_t bufsz) {
#ifdef _WIN32
    if (!_getcwd(buf, (int)bufsz))
        return false;
    for (char *p = buf; *p; p++)
        if (*p == '\\')
            *p = '/';
    return true;
#else
    return getcwd(buf, bufsz) != NULL;
#endif
}

bool plat_stderr_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

bool plat_realpath(const char *path, char *out, size_t outsz) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    if (!_fullpath(out, winpath(wb, sizeof wb, path), outsz))
        return false;
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    return plat_is_dir(out) || plat_is_file(out);
#else
    char buf[PATH_MAX];
    if (!realpath(path, buf))
        return false;
    return snprintf(out, outsz, "%s", buf) < (int)outsz;
#endif
}

static FILE *open_rb(const char *path) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    return fopen(winpath(wb, sizeof wb, path), "rb");
#else
    return fopen(path, "rb");
#endif
}

bool plat_read_file_max(Arena *a, const char *path, char **data, size_t *len,
                        size_t max_size) {
    FILE *f = open_rb(path);
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > max_size) {
        fclose(f);
        return false;
    }
    rewind(f);
    char *buf = (char *)arena_alloc(a, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz)
        return false;
    buf[sz] = '\0';
    *data = buf;
    *len = (size_t)sz;
    return true;
}

bool plat_read_file(Arena *a, const char *path, char **data, size_t *len) {
    return plat_read_file_max(a, path, data, len, KB_MAX_FILE_SIZE);
}

bool plat_read_tail(Arena *a, const char *path, size_t want, char **data,
                    size_t *len, uint64_t *file_size) {
    FILE *f = open_rb(path);
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return false;
    }
    *file_size = (uint64_t)sz;
    size_t take = (size_t)sz < want ? (size_t)sz : want;
    if (fseek(f, (long)((size_t)sz - take), SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    char *buf = (char *)arena_alloc(a, take + 1);
    size_t got = fread(buf, 1, take, f);
    fclose(f);
    if (got != take)
        return false;
    buf[take] = '\0';
    *data = buf;
    *len = take;
    return true;
}

bool plat_read_range(Arena *a, const char *path, uint64_t off, size_t len,
                     char **data) {
    FILE *f = open_rb(path);
    if (!f)
        return false;
    if (fseek(f, (long)off, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    char *buf = (char *)arena_alloc(a, len + 1);
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len)
        return false;
    buf[len] = '\0';
    *data = buf;
    return true;
}

bool plat_fsync(FILE *f) {
    if (fflush(f) != 0)
        return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

bool plat_tty_ansi(FILE *f) {
#ifdef _WIN32
    int fd = _fileno(f);
    if (fd < 0 || !_isatty(fd))
        return false;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD mode;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode))
        return false;
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
           SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    return isatty(fileno(f)) == 1;
#endif
}

bool plat_is_tmp_name(const char *name) {
    const char *p = strstr(name, ".tmp.");
    if (!p)
        return false;
    for (p += 5; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return p[-1] >= '0' && p[-1] <= '9';
}

bool plat_file_size(const char *path, uint64_t *size) {
    FILE *f = open_rb(path);
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    fclose(f);
    if (sz < 0)
        return false;
    *size = (uint64_t)sz;
    return true;
}

/* ---- read-only whole-file mapping -------------------------------------- */

struct PlatMap {
    void *base;
    size_t len;
#ifdef _WIN32
    HANDLE file;
    HANDLE mapping;
#endif
};

PlatMap *plat_map_file(Arena *a, const char *path, const uint8_t **base,
                       size_t *len) {
    *base = NULL;
    *len = 0;
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    HANDLE fh = CreateFileA(winpath(wb, sizeof wb, path), GENERIC_READ,
                            FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(fh, &sz) || sz.QuadPart <= 0 ||
        (uint64_t)sz.QuadPart > (uint64_t)SIZE_MAX) {
        CloseHandle(fh);
        return NULL;
    }
    HANDLE mh = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mh) {
        CloseHandle(fh);
        return NULL;
    }
    void *p = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (!p) {
        CloseHandle(mh);
        CloseHandle(fh);
        return NULL;
    }
    PlatMap *m = (PlatMap *)arena_alloc(a, sizeof(PlatMap));
    m->base = p;
    m->len = (size_t)sz.QuadPart;
    m->file = fh;
    m->mapping = mh;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    /* The descriptor has done its job: a mapping keeps the file alive on its
     * own, and holding the fd as well would leak one per model load. */
    close(fd);
    if (p == MAP_FAILED)
        return NULL;
    PlatMap *m = (PlatMap *)arena_alloc(a, sizeof(PlatMap));
    m->base = p;
    m->len = (size_t)st.st_size;
#endif
    *base = (const uint8_t *)m->base;
    *len = m->len;
    return m;
}

void plat_unmap_file(PlatMap *m) {
    if (!m || !m->base)
        return;
#ifdef _WIN32
    UnmapViewOfFile(m->base);
    CloseHandle(m->mapping);
    CloseHandle(m->file);
#else
    munmap(m->base, m->len);
#endif
    m->base = NULL;
    m->len = 0;
}

bool plat_truncate(const char *path, uint64_t new_size) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    winpath(wb, sizeof wb, path);
    HANDLE h = CreateFileA(wb, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)new_size;
    bool ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) &&
              SetEndOfFile(h);
    CloseHandle(h);
    return ok;
#else
    return truncate(path, (off_t)new_size) == 0;
#endif
}

bool plat_write_file_atomic(const char *path, const void *data, size_t len) {
    char tmp[KB_PATH_MAX];
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    /* per-process temp name: concurrent writers to the same target never
     * clobber each other's half-written temp file */
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%lu", path, pid) >=
        (int)sizeof tmp)
        return false;
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    FILE *f = fopen(winpath(wb, sizeof wb, tmp), "wb");
#else
    FILE *f = fopen(tmp, "wb");
#endif
    if (!f)
        return false;
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(f)) == 0 && ok;
#else
    ok = fsync(fileno(f)) == 0 && ok;
#endif
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        plat_remove_file(tmp);
        return false;
    }
#ifdef _WIN32
    char wsrc[KB_PATH_MAX], wdst[KB_PATH_MAX];
    if (!MoveFileExA(winpath(wsrc, sizeof wsrc, tmp),
                     winpath(wdst, sizeof wdst, path),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        plat_remove_file(tmp);
        return false;
    }
    return true;
#else
    if (rename(tmp, path) != 0) {
        plat_remove_file(tmp);
        return false;
    }
    return true;
#endif
}

bool plat_append_file_sync(const char *path, const void *data, size_t len) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    FILE *f = fopen(winpath(wb, sizeof wb, path), "ab");
#else
    FILE *f = fopen(path, "ab");
#endif
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(f)) == 0 && ok;
#else
    ok = fsync(fileno(f)) == 0 && ok;
#endif
    ok = fclose(f) == 0 && ok;
    return ok;
}

int64_t plat_now_epoch(void) {
    return (int64_t)time(NULL);
}

void plat_time_format(int64_t epoch, char out[32]) {
    time_t t = (time_t)epoch;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void plat_timestamp(char out[32]) {
    plat_time_format(plat_now_epoch(), out);
}

/* Days from 1970-01-01 to a proleptic-Gregorian y/m/d. Hinnant's
 * days_from_civil: exact for every representable date, no loops, no tables,
 * and no dependence on the platform's timezone database — which matters
 * because gmtime is a one-way street and timegm is not in C11. */
static int64_t days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);   /* year of era, [0, 399] */
    uint32_t mp = m > 2 ? m - 3u : m + 9u;      /* March-first month, [0, 11] */
    uint32_t doy = (153u * mp + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t days_in_month(int64_t y, uint32_t m) {
    static const uint8_t len[13] = {0,  31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return len[m];
}

/* Two digits at p, or -1. Written out rather than left to sscanf, which
 * would accept " 7", "+7" and a field that runs past its width. */
static int32_t two_digits(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9')
        return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

bool plat_time_parse(const char *iso, int64_t *out) {
    if (!iso || strlen(iso) != 20)
        return false;
    if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' ||
        iso[16] != ':' || iso[19] != 'Z')
        return false;
    int32_t hi = two_digits(iso), lo = two_digits(iso + 2);
    int32_t mo = two_digits(iso + 5), d = two_digits(iso + 8);
    int32_t h = two_digits(iso + 11), mi = two_digits(iso + 14);
    int32_t s = two_digits(iso + 17);
    if (hi < 0 || lo < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || s < 0)
        return false;
    int64_t y = (int64_t)hi * 100 + lo;
    if (mo < 1 || mo > 12 || h > 23 || mi > 59 || s > 59)
        return false;
    if (d < 1 || (uint32_t)d > days_in_month(y, (uint32_t)mo))
        return false;
    *out = days_from_civil(y, (uint32_t)mo, (uint32_t)d) * 86400 +
           (int64_t)h * 3600 + (int64_t)mi * 60 + s;
    return true;
}

/* ---- directory walk ---- */

typedef struct {
    char *name;
    bool is_dir;
} Entry;

typedef struct {
    Entry *v;
    size_t n, cap;
} EntryList;

static int entry_cmp(const void *pa, const void *pb) {
    return strcmp(((const Entry *)pa)->name, ((const Entry *)pb)->name);
}

static bool walk_dir(Arena *a, const char *root, const char *rel, WalkFn fn,
                     void *ud) {
    char full[KB_PATH_MAX];
    if (rel[0])
        snprintf(full, sizeof full, "%s/%s", root, rel);
    else
        snprintf(full, sizeof full, "%s", root);

    EntryList el = {NULL, 0, 0};

#ifdef _WIN32
    char pattern[KB_PATH_MAX];
    char wb[KB_PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s/*", full);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(winpath(wb, sizeof wb, pattern), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return true; /* unreadable dir: skip quietly */
    do {
        const char *name = fd.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue; /* symlink/junction: skip */
        ARENA_GROW(a, el.v, el.n, el.cap, Entry);
        el.v[el.n].name = arena_strdup(a, name);
        el.v[el.n].is_dir =
            (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        el.n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(full);
    if (!d)
        return true; /* unreadable dir: skip quietly */
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[KB_PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", full, name);
        struct stat st;
        if (lstat(child, &st) != 0)
            continue;
        if (S_ISLNK(st.st_mode))
            continue;
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
            continue;
        ARENA_GROW(a, el.v, el.n, el.cap, Entry);
        el.v[el.n].name = arena_strdup(a, name);
        el.v[el.n].is_dir = S_ISDIR(st.st_mode);
        el.n++;
    }
    closedir(d);
#endif

    if (el.n > 1)
        qsort(el.v, el.n, sizeof(Entry), entry_cmp);

    for (size_t i = 0; i < el.n; i++) {
        char childrel[KB_PATH_MAX];
        if (rel[0])
            snprintf(childrel, sizeof childrel, "%s/%s", rel, el.v[i].name);
        else
            snprintf(childrel, sizeof childrel, "%s", el.v[i].name);
        if (el.v[i].is_dir) {
            WalkAction act = fn(childrel, true, ud);
            if (act == WALK_SKIP_DIR)
                continue;
            if (!walk_dir(a, root, childrel, fn, ud))
                return false;
        } else {
            fn(childrel, false, ud);
        }
    }
    return true;
}

bool plat_walk(Arena *a, const char *root, WalkFn fn, void *ud) {
    return walk_dir(a, root, "", fn, ud);
}

/* ---- locking ---- */

struct PlatLock {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
};

PlatLock *plat_lock(Arena *a, const char *path) {
    PlatLock *l = (PlatLock *)arena_alloc0(a, sizeof(PlatLock));
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    winpath(wb, sizeof wb, path);
    /* Exclusive create-or-open with no sharing; retry up to ~10s. */
    for (int i = 0; i < 200; i++) {
        HANDLE h = CreateFileA(wb, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            l->h = h;
            return l;
        }
        Sleep(50);
    }
    return NULL;
#else
    int fd = open(path, O_CREAT | O_WRONLY, 0666);
    if (fd < 0)
        return NULL;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return NULL;
    }
    l->fd = fd;
    return l;
#endif
}

void plat_unlock(PlatLock *l) {
    if (!l)
        return;
#ifdef _WIN32
    CloseHandle(l->h);
#else
    flock(l->fd, LOCK_UN);
    close(l->fd);
#endif
}

/* ---- additions kb needs beyond lap's platform layer -------------------- */

int64_t plat_pid(void) {
#ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
#else
    return (int64_t)getpid();
#endif
}

bool plat_remove_dir(const char *path) {
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    return _rmdir(winpath(wb, sizeof wb, path)) == 0;
#else
    return rmdir(path) == 0;
#endif
}

/* A lock that REFUSES rather than waits, and names who holds it.
 *
 * lap's `plat_lock` blocks until it wins, which is right for a tool whose
 * commands are short. It is wrong here: `index-api.md` §11 makes `store_locked`
 * an error with "the holding process" as its detail, so the caller has to be
 * able to fail and say who. A blocking lock can only ever hang.
 *
 * The pid is written into the lock file rather than inferred, because there is
 * no portable way to ask the kernel who holds a flock. That makes the pid
 * advisory: a stale file from a crashed writer still names a dead process. It
 * is reported as a hint in an error message and nothing decides anything on
 * it — the flock itself is the authority, and a crashed process releases that
 * when its descriptors close.
 *
 * `*holder` distinguishes three outcomes the caller renders differently:
 * negative, the lock file could not be opened at all; positive, held by that
 * pid; zero, held but by whom we could not say. */
PlatLock *plat_lock_try(Arena *a, const char *path, int64_t *holder) {
    *holder = -1;
    PlatLock *l = (PlatLock *)arena_alloc0(a, sizeof(PlatLock));
#ifdef _WIN32
    char wb[KB_PATH_MAX];
    winpath(wb, sizeof wb, path);
    HANDLE h = CreateFileA(wb, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        /* Sharing violation means somebody has it; anything else means we
         * could not even ask. */
        *holder = GetLastError() == ERROR_SHARING_VIOLATION ? 0 : -1;
        return NULL;
    }
    char buf[32];
    int32_t n = snprintf(buf, sizeof buf, "%lld\n", (long long)plat_pid());
    DWORD wrote = 0;
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    WriteFile(h, buf, (DWORD)n, &wrote, NULL);
    l->h = h;
    return l;
#else
    int fd = open(path, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return NULL;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        /* Held. Read the pid the holder stamped, if it left one. */
        *holder = 0;
        char buf[32];
        ssize_t got = pread(fd, buf, sizeof buf - 1, 0);
        if (got > 0) {
            buf[got] = '\0';
            long long pid = strtoll(buf, NULL, 10);
            if (pid > 0)
                *holder = (int64_t)pid;
        }
        close(fd);
        return NULL;
    }
    /* Truncate before writing: a shorter pid must not leave a longer one's
     * trailing digits behind, which would name a process that never held it. */
    if (ftruncate(fd, 0) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return NULL;
    }
    char buf[32];
    int32_t n = snprintf(buf, sizeof buf, "%lld\n", (long long)plat_pid());
    if (n > 0 && write(fd, buf, (size_t)n) != n) {
        flock(fd, LOCK_UN);
        close(fd);
        return NULL;
    }
    l->fd = fd;
    return l;
#endif
}

/* ---- parallel work ---- */

#define PLAT_MAX_THREADS 64

/* Ranges handed out per thread, roughly: small enough that a fast core takes
 * more of them than a slow one, large enough that asking costs nothing next
 * to the work. */
#define PLAT_SLICES_PER_THREAD 8

static size_t grain_for(size_t n, size_t threads) {
    size_t g = n / (threads * PLAT_SLICES_PER_THREAD);
    return g ? g : 1;
}

#ifdef _WIN32
size_t plat_cpus(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (size_t)si.dwNumberOfProcessors : 1;
}

/* Windows starts threads per call, as before, but they take ranges from a
 * shared counter like the pool below does. */
typedef struct {
    PlatRangeFn fn;
    void *ud;
    size_t n, grain;
    volatile LONG64 next;
} WinJob;

static void win_take(WinJob *j) {
    for (;;) {
        size_t b = (size_t)InterlockedExchangeAdd64(&j->next, (LONG64)j->grain);
        if (b >= j->n)
            return;
        size_t e = b + j->grain < j->n ? b + j->grain : j->n;
        j->fn(b, e, j->ud);
    }
}

static DWORD WINAPI win_main(LPVOID p) {
    win_take((WinJob *)p);
    return 0;
}

void plat_parallel(size_t n, PlatRangeFn fn, void *ud) {
    size_t k = plat_cpus();
    if (k > PLAT_MAX_THREADS)
        k = PLAT_MAX_THREADS;
    if (k > n)
        k = n;
    if (k <= 1) {
        fn(0, n, ud);
        return;
    }
    WinJob job = {fn, ud, n, grain_for(n, k), 0};
    HANDLE th[PLAT_MAX_THREADS];
    for (size_t i = 1; i < k; i++)
        th[i] = CreateThread(NULL, 0, win_main, &job, 0, NULL);
    win_take(&job);
    for (size_t i = 1; i < k; i++) {
        if (th[i]) {
            WaitForSingleObject(th[i], INFINITE);
            CloseHandle(th[i]);
        }
    }
}
#else
#include <pthread.h>
#include <stdatomic.h>

size_t plat_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (size_t)n : 1;
}

/* THE POOL. Workers wait for a new generation, take ranges from `next` until
 * none are left, and report in through `busy`. The caller takes ranges too,
 * then waits until every worker has reported, so no worker can still be
 * reading this job's fields when the next job overwrites them.
 *
 * Between jobs a worker spins for a moment before it sleeps: in a forward
 * pass the next job follows within microseconds, and a sleeping thread takes
 * longer than that to wake. */
#define PLAT_SPIN 20000

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t wake, done;
    size_t workers;
    PlatRangeFn fn;
    void *ud;
    size_t n, grain;
    atomic_size_t next;
    atomic_size_t busy;
    atomic_uint_fast64_t gen;
} Pool;

static Pool pool = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                    PTHREAD_COND_INITIALIZER, 0, NULL, NULL, 0, 0, 0, 0, 0};
static pthread_once_t pool_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t pool_user = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool in_range;

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#endif
}

static void take_ranges(void) {
    in_range = true;
    for (;;) {
        size_t b = atomic_fetch_add(&pool.next, pool.grain);
        if (b >= pool.n)
            break;
        size_t e = b + pool.grain < pool.n ? b + pool.grain : pool.n;
        pool.fn(b, e, pool.ud);
    }
    in_range = false;
}

static void *worker_main(void *unused) {
    (void)unused;
    uint_fast64_t seen = 0;
    for (;;) {
        uint_fast64_t g = atomic_load(&pool.gen);
        for (int32_t i = 0; g == seen && i < PLAT_SPIN; i++) {
            cpu_relax();
            g = atomic_load(&pool.gen);
        }
        if (g == seen) {
            pthread_mutex_lock(&pool.mu);
            while ((g = atomic_load(&pool.gen)) == seen)
                pthread_cond_wait(&pool.wake, &pool.mu);
            pthread_mutex_unlock(&pool.mu);
        }
        seen = g;
        take_ranges();
        if (atomic_fetch_sub(&pool.busy, 1) == 1) {
            pthread_mutex_lock(&pool.mu);
            pthread_cond_signal(&pool.done);
            pthread_mutex_unlock(&pool.mu);
        }
    }
    return NULL;
}

static void pool_start(void) {
    size_t k = plat_cpus();
    if (k > PLAT_MAX_THREADS)
        k = PLAT_MAX_THREADS;
    for (size_t i = 1; i < k; i++) {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        bool ok = pthread_create(&th, &attr, worker_main, NULL) == 0;
        pthread_attr_destroy(&attr);
        if (!ok)
            break;
        pool.workers++;
    }
}

void plat_parallel(size_t n, PlatRangeFn fn, void *ud) {
    if (n <= 1 || in_range) {
        fn(0, n, ud);
        return;
    }
    pthread_once(&pool_once, pool_start);
    if (pool.workers == 0 || pthread_mutex_trylock(&pool_user) != 0) {
        fn(0, n, ud);
        return;
    }
    pthread_mutex_lock(&pool.mu);
    pool.fn = fn;
    pool.ud = ud;
    pool.n = n;
    pool.grain = grain_for(n, pool.workers + 1);
    atomic_store(&pool.next, 0);
    atomic_store(&pool.busy, pool.workers);
    atomic_fetch_add(&pool.gen, 1);
    pthread_cond_broadcast(&pool.wake);
    pthread_mutex_unlock(&pool.mu);

    take_ranges();

    for (int32_t i = 0; atomic_load(&pool.busy) != 0 && i < PLAT_SPIN; i++)
        cpu_relax();
    if (atomic_load(&pool.busy) != 0) {
        pthread_mutex_lock(&pool.mu);
        while (atomic_load(&pool.busy) != 0)
            pthread_cond_wait(&pool.done, &pool.mu);
        pthread_mutex_unlock(&pool.mu);
    }
    pthread_mutex_unlock(&pool_user);
}
#endif
