#include "platform.h"

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
#include <sys/file.h>
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
    char wb[LAP_PATH_MAX];
    DWORD attr = GetFileAttributesA(winpath(wb, sizeof wb, path));
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

#ifdef _WIN32
/* FILETIME counts 100 ns ticks from 1601; the stat cache wants the epoch. */
static void filetime_to(PlatStat *out, FILETIME ft, DWORD hi, DWORD lo) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    out->mtime_sec = (int64_t)(t / 10000000ULL);
    out->mtime_nsec = (int32_t)((t % 10000000ULL) * 100);
    out->size = ((uint64_t)hi << 32) | lo;
}
#elif defined(__APPLE__)
#define ST_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#else
#define ST_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#endif

#ifndef _WIN32
static void stat_to(PlatStat *out, const struct stat *st) {
    out->size = (uint64_t)st->st_size;
    out->mtime_sec = (int64_t)st->st_mtime;
    out->mtime_nsec = (int32_t)ST_MTIME_NSEC(*st);
}
#endif

int64_t plat_now_sec(void) {
    return (int64_t)time(NULL);
}

/* struct tm read as UTC, whatever the machine's zone. */
static int64_t utc_seconds(struct tm *tm) {
#ifdef _WIN32
    return (int64_t)_mkgmtime(tm);
#else
    return (int64_t)timegm(tm);
#endif
}

static void local_tm(time_t t, struct tm *out) {
#ifdef _WIN32
    localtime_s(out, &t);
#else
    localtime_r(&t, out);
#endif
}

void plat_ts_local(const char *utc, bool with_offset, char out[40]) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(utc, "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon,
               &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        snprintf(out, 40, "%s", utc);
        return;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    time_t t = (time_t)utc_seconds(&tm);
    struct tm loc;
    local_tm(t, &loc);
    size_t n = strftime(out, 40, "%Y-%m-%d %H:%M:%S", &loc);
    if (with_offset) {
        /* the local fields read as UTC, less the instant, is the offset */
        struct tm again = loc;
        long off = (long)(utc_seconds(&again) - (int64_t)t);
        long a = off < 0 ? -off : off;
        snprintf(out + n, 40 - n, " %c%02ld:%02ld", off < 0 ? '-' : '+',
                 a / 3600, a / 60 % 60);
    }
}

bool plat_ts_parse(const char *in, char out[32]) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int used = 0;
    if (sscanf(in, "%4d-%2d-%2d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &used) != 3)
        return false;
    const char *p = in + used;
    if (*p == 'T' || *p == ' ') {
        int h, m, n = 0;
        if (sscanf(p + 1, "%2d:%2d%n", &h, &m, &n) != 2)
            return false;
        tm.tm_hour = h;
        tm.tm_min = m;
        p += 1 + n;
        if (*p == ':') {
            int s;
            if (sscanf(p + 1, "%2d%n", &s, &n) != 1)
                return false;
            tm.tm_sec = s;
            p += 1 + n;
        }
    }
    if (tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 ||
        tm.tm_mday > 31 || tm.tm_hour > 23 || tm.tm_min > 59 ||
        tm.tm_sec > 60)
        return false;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    int64_t t;
    if (*p == 'Z' && p[1] == '\0') {
        t = utc_seconds(&tm);
    } else if ((*p == '+' || *p == '-') && p[1]) {
        int oh, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 1 &&
            sscanf(p + 1, "%2d%2d", &oh, &om) < 1)
            return false;
        long off = (long)oh * 3600 + (long)om * 60;
        t = utc_seconds(&tm) - (*p == '-' ? -off : off);
    } else if (*p == '\0') {
        /* local: of the moments these wall-clock fields can name (two in
         * the repeated hour), the earlier */
        struct tm a = tm, b = tm;
        a.tm_isdst = 1;
        b.tm_isdst = 0;
        time_t ta = mktime(&a), tb = mktime(&b);
        struct tm la, lb;
        local_tm(ta, &la);
        local_tm(tb, &lb);
        bool ok_a = ta != (time_t)-1 && la.tm_hour == tm.tm_hour &&
                    la.tm_min == tm.tm_min && la.tm_mday == tm.tm_mday;
        bool ok_b = tb != (time_t)-1 && lb.tm_hour == tm.tm_hour &&
                    lb.tm_min == tm.tm_min && lb.tm_mday == tm.tm_mday;
        if (ok_a && ok_b)
            t = (int64_t)(ta < tb ? ta : tb);
        else if (ok_a || ok_b)
            t = (int64_t)(ok_a ? ta : tb);
        else {
            struct tm c = tm;
            c.tm_isdst = -1;
            time_t tc = mktime(&c); /* a skipped hour: whatever it maps to */
            if (tc == (time_t)-1)
                return false;
            t = (int64_t)tc;
        }
    } else {
        return false;
    }
    time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &g);
    return true;
}

bool plat_is_dir(const char *path) {
#ifdef _WIN32
    char wb[LAP_PATH_MAX];
    DWORD attr = GetFileAttributesA(winpath(wb, sizeof wb, path));
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool plat_mkdir(const char *path) {
#ifdef _WIN32
    char wb[LAP_PATH_MAX];
    if (_mkdir(winpath(wb, sizeof wb, path)) == 0)
        return true;
    return plat_is_dir(path);
#else
    if (mkdir(path, 0777) == 0)
        return true;
    return errno == EEXIST && plat_is_dir(path);
#endif
}

bool plat_mkdirs(const char *path) {
    char buf[LAP_PATH_MAX];
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
    char wb[LAP_PATH_MAX];
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

static FILE *open_rb(const char *path) {
#ifdef _WIN32
    char wb[LAP_PATH_MAX];
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
    return plat_read_file_max(a, path, data, len, LAP_MAX_FILE_SIZE);
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

bool plat_read_range_into(const char *path, uint64_t off, size_t len,
                          char *buf) {
    FILE *f = open_rb(path);
    if (!f)
        return false;
    if (fseek(f, (long)off, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len;
}

bool plat_read_range(Arena *a, const char *path, uint64_t off, size_t len,
                     char **data) {
    char *buf = (char *)arena_alloc(a, len + 1);
    if (!plat_read_range_into(path, off, len, buf))
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

bool plat_truncate(const char *path, uint64_t new_size) {
#ifdef _WIN32
    char wb[LAP_PATH_MAX];
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
    char tmp[LAP_PATH_MAX];
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
    char wb[LAP_PATH_MAX];
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
    char wsrc[LAP_PATH_MAX], wdst[LAP_PATH_MAX];
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
    char wb[LAP_PATH_MAX];
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

void plat_timestamp(char out[32]) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

/* ---- directory walk ---- */

typedef struct {
    char *name;
    bool is_dir;
    PlatStat st;
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
    char full[LAP_PATH_MAX];
    if (rel[0])
        snprintf(full, sizeof full, "%s/%s", root, rel);
    else
        snprintf(full, sizeof full, "%s", root);

    EntryList el = {NULL, 0, 0};

#ifdef _WIN32
    char pattern[LAP_PATH_MAX];
    char wb[LAP_PATH_MAX];
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
        filetime_to(&el.v[el.n].st, fd.ftLastWriteTime, fd.nFileSizeHigh,
                    fd.nFileSizeLow);
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
        char child[LAP_PATH_MAX];
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
        stat_to(&el.v[el.n].st, &st);
        el.n++;
    }
    closedir(d);
#endif

    if (el.n > 1)
        qsort(el.v, el.n, sizeof(Entry), entry_cmp);

    for (size_t i = 0; i < el.n; i++) {
        char childrel[LAP_PATH_MAX];
        if (rel[0])
            snprintf(childrel, sizeof childrel, "%s/%s", rel, el.v[i].name);
        else
            snprintf(childrel, sizeof childrel, "%s", el.v[i].name);
        if (el.v[i].is_dir) {
            WalkAction act = fn(childrel, true, &el.v[i].st, ud);
            if (act == WALK_SKIP_DIR)
                continue;
            if (!walk_dir(a, root, childrel, fn, ud))
                return false;
        } else {
            fn(childrel, false, &el.v[i].st, ud);
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
    char wb[LAP_PATH_MAX];
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

PlatLock *plat_trylock(Arena *a, const char *path) {
    PlatLock *l = (PlatLock *)arena_alloc0(a, sizeof(PlatLock));
#ifdef _WIN32
    char wb[LAP_PATH_MAX];
    HANDLE h = CreateFileA(winpath(wb, sizeof wb, path), GENERIC_WRITE, 0,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    l->h = h;
    return l;
#else
    int fd = open(path, O_CREAT | O_WRONLY, 0666);
    if (fd < 0)
        return NULL;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
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
