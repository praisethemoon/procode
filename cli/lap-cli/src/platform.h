/* OS abstraction: files, directories, locking, timestamps.
 * POSIX is the tested path; the _WIN32 branch is best-effort.
 * All paths use '/' separators internally; the Windows branch converts.
 */
#ifndef LAP_PLATFORM_H
#define LAP_PLATFORM_H

#include "arena.h"

bool plat_is_file(const char *path);
bool plat_is_dir(const char *path);
/* True when both paths reach one file or directory, however spelled:
 * case, symlinks, "..". False when either does not exist. */
bool plat_same_file(const char *a, const char *b);
/* An existing path's canonical absolute form: symlinks resolved, '/'
 * separators. False when it does not exist. */
bool plat_realpath(const char *path, char *out, size_t outsz);
/* Renames a file or a directory, atomically, onto a name that is free. */
bool plat_rename(const char *from, const char *to);
/* Removes an empty directory. */
bool plat_rmdir(const char *path);
bool plat_mkdir(const char *path);       /* ok if already exists */
bool plat_mkdirs(const char *path);      /* mkdir -p */
bool plat_remove_file(const char *path);
bool plat_getcwd(char *buf, size_t bufsz);

/* Reads the whole file (binary-safe). *data gets a NUL-terminated buffer,
 * *len the byte count excluding the NUL. Returns false if unreadable or
 * larger than LAP_MAX_FILE_SIZE (user-file safety cap).
 */
bool plat_read_file(Arena *a, const char *path, char **data, size_t *len);
/* Same, with an explicit cap; pass (size_t)-1 for lap's own uncapped files
 * (the log must never become unreadable by growing).
 */
bool plat_read_file_max(Arena *a, const char *path, char **data, size_t *len,
                        size_t max_size);
/* Reads the final `want` bytes (or the whole file if smaller). Fills
 * *file_size with the total size. NUL-terminated like plat_read_file.
 */
bool plat_read_tail(Arena *a, const char *path, size_t want, char **data,
                    size_t *len, uint64_t *file_size);
/* Truncates the file to new_size bytes. */
bool plat_truncate(const char *path, uint64_t new_size);
/* Reads exactly [off, off+len) into a NUL-terminated buffer. */
bool plat_read_range(Arena *a, const char *path, uint64_t off, size_t len,
                     char **data);
/* Reads exactly [off, off+len) into buf, which holds at least len bytes. */
bool plat_read_range_into(const char *path, uint64_t off, size_t len,
                          char *buf);
/* File size in bytes; false if the file does not exist. */
bool plat_file_size(const char *path, uint64_t *size);
/* Flushes a stream all the way to disk. */
bool plat_fsync(FILE *f);
/* True when f is an interactive terminal that renders ANSI styling. On
 * Windows this also switches the console into VT mode; a console that
 * refuses is reported as not styleable. */
bool plat_tty_ansi(FILE *f);
/* True for an atomic-write temp file ("<name>.tmp.<pid>"), which cache
 * walks must ignore — a crashed write can leave one behind. */
bool plat_is_tmp_name(const char *name);
/* Writes via a per-process temp file + atomic rename. */
bool plat_write_file_atomic(const char *path, const void *data, size_t len);
/* Appends and flushes to disk before returning. */
bool plat_append_file_sync(const char *path, const void *data, size_t len);

/* UTC ISO-8601, e.g. "2026-09-20T12:34:56Z". */
void plat_timestamp(char out[32]);

/* Directory walk, depth-first, entries sorted by name for determinism.
 * The callback sees repo-relative paths with '/' separators. Directories are
 * reported before their contents; returning WALK_SKIP_DIR prunes descent.
 * Symbolic links are skipped entirely.
 */
typedef enum { WALK_CONT, WALK_SKIP_DIR } WalkAction;
/* A file's size and modification time: what a stat cache compares.
 * mtime_nsec is 0 where the platform keeps whole seconds only. */
typedef struct {
    uint64_t size;
    int64_t mtime_sec;
    int32_t mtime_nsec;
} PlatStat;
/* st is the entry's own stat, taken by the walk anyway: a caller that keeps
 * it needs no second stat of the file. */
typedef WalkAction (*WalkFn)(const char *rel, bool is_dir, const PlatStat *st,
                             void *ud);
bool plat_walk(Arena *a, const char *root, WalkFn fn, void *ud);
/* A regular file's stat, as the walk takes it; false for anything else. */
bool plat_stat(const char *path, PlatStat *out);
/* Seconds since the epoch, now. */
int64_t plat_now_sec(void);
/* A log timestamp (UTC, "2026-09-27T09:36:48Z") in this machine's zone for
 * people: "2026-09-27 11:36:48", then " +02:00" when with_offset. Input it
 * cannot read is copied through unchanged. */
void plat_ts_local(const char *utc, bool with_offset, char out[40]);
/* A time as a person writes it, as a log timestamp. With a "Z" or an offset
 * it is taken as given; without one it is local time. Accepts a date, or a
 * date and time ("T" or a space between, seconds optional). In the hour
 * that repeats when clocks go back, the earlier moment is taken. False when
 * it is not a time. */
bool plat_ts_parse(const char *in, char out[32]);

/* Exclusive advisory lock via a lock file. Returns a handle or NULL on
 * failure (waits for a competing holder, with a bounded retry on Windows).
 */
typedef struct PlatLock PlatLock;
PlatLock *plat_lock(Arena *a, const char *path);
/* The same lock, only if it is free right now: NULL when another process
 * holds it, never waiting. */
PlatLock *plat_trylock(Arena *a, const char *path);
void plat_unlock(PlatLock *l);

#endif /* LAP_PLATFORM_H */
