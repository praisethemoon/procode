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
/* Whether standard error is a terminal: progress lines are for a person
 * watching, not for a log. */
bool plat_stderr_tty(void);
/* The canonical absolute path of an existing path, symbolic links resolved
 * and separators as '/'. False when it does not exist. */
bool plat_realpath(const char *path, char *out, size_t outsz);

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

/* ---- read-only whole-file mapping ----
 *
 * Every other reader here copies into the arena, which is right for a log or
 * a blob: they are small, they are parsed once, and a copy is simpler than a
 * mapping. The embedding model is neither — it is tens of megabytes of
 * quantised weights that the forward pass reads over and over and never
 * writes. Copying it would spend the memory twice and the time once for
 * nothing, so it is mapped instead, and the pages stay shared with every
 * other process that has the same model open.
 *
 * The mapping is READ-ONLY and PRIVATE: kb must not be able to write through
 * it even by accident, and nothing kb does may reach the user's model file.
 *
 * Returns NULL on failure (including an empty file, which cannot be mapped
 * and is never a model anyway). *len gets the size on success.
 */
typedef struct PlatMap PlatMap;
PlatMap *plat_map_file(Arena *a, const char *path, const uint8_t **base,
                       size_t *len);
void plat_unmap_file(PlatMap *m);
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

/* ---- absolute time ----
 *
 * Everywhere else in kb a timestamp is a string and "newer than" is a string
 * comparison, which is exact and needs no calendar (§4's `since`). §5's
 * `olderThan` is a DURATION, and subtracting 90 days from a date is calendar
 * arithmetic however it is spelled — so these three exist, and nothing else
 * in kb converts a time.
 *
 * plat_timestamp is plat_time_format(plat_now_epoch()), so there is exactly
 * one spelling of a kb timestamp and a value this parses is a value that
 * formatter wrote.
 */
int64_t plat_now_epoch(void);
void plat_time_format(int64_t epoch, char out[32]);
/* Strict: exactly the 20 bytes plat_time_format writes, a real calendar date,
 * and nothing else. Every value it sees came out of a log line a person can
 * edit, and a lenient parser would silently reinterpret "2026-02-31" rather
 * than say it cannot read it. */
bool plat_time_parse(const char *iso, int64_t *out);

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

/* ---- parallel work ----
 *
 * Runs fn over [0, n) split into contiguous ranges, one per thread, and
 * returns when every range is done. The only concurrency in kb, and only for
 * embedding, where a chunk is tens of billions of multiply-adds. A caller
 * must make every index's work independent of every other's — each output
 * written by exactly one range — so the result cannot depend on how many
 * threads there were. Falls back to running fn(0, n) inline when threads
 * cannot be started. */
typedef void (*PlatRangeFn)(size_t begin, size_t end, void *ud);
void plat_parallel(size_t n, PlatRangeFn fn, void *ud);
/* Logical CPUs, at least 1. */
size_t plat_cpus(void);

#endif /* KB_PLATFORM_H */
