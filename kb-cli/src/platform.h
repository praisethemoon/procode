/* OS abstraction: files, directories, locking, timestamps.
 * POSIX is the tested path; the _WIN32 branch is best-effort.
 * All paths use '/' separators internally; the Windows branch converts.
 */
#ifndef KB_PLATFORM_H
#define KB_PLATFORM_H

#include "arena.h"

bool plat_is_file(const char *path);
bool plat_is_dir(const char *path);
bool plat_mkdir(const char *path);       /* ok if already exists */
bool plat_mkdirs(const char *path);      /* mkdir -p */
bool plat_remove_file(const char *path);
bool plat_remove_dir(const char *path); /* must already be empty */
bool plat_getcwd(char *buf, size_t bufsz);

/* Reads the whole file (binary-safe). *data gets a NUL-terminated buffer,
 * *len the byte count excluding the NUL. Returns false if unreadable or
 * larger than KB_MAX_FILE_SIZE (user-file safety cap).
 */
bool plat_read_file(Arena *a, const char *path, char **data, size_t *len);
/* Same, with an explicit cap; pass (size_t)-1 for kb's own uncapped files
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
typedef WalkAction (*WalkFn)(const char *rel, bool is_dir, void *ud);
bool plat_walk(Arena *a, const char *root, WalkFn fn, void *ud);

/* Exclusive advisory lock via a lock file. Returns a handle or NULL on
 * failure (waits for a competing holder, with a bounded retry on Windows).
 */
typedef struct PlatLock PlatLock;
PlatLock *plat_lock(Arena *a, const char *path);
void plat_unlock(PlatLock *l);

/* This process's id. */
int64_t plat_pid(void);

/* Non-blocking variant. Returns NULL without waiting when someone else holds
 * the lock, and writes the holder's pid to *holder (0 when it cannot be
 * determined) so the caller can name it — a writer that just prints "locked"
 * leaves the reader with nothing to act on. On success the holder's own pid
 * is stamped into the lock file for exactly that reason.
 *
 * *holder is set to -1 when the failure was not contention (the lock file
 * could not be created at all), which is an error of a different kind.
 */
PlatLock *plat_lock_try(Arena *a, const char *path, int64_t *holder);

#endif /* KB_PLATFORM_H */
