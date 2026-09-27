/* The stat cache: .lap/statcache, what makes `lap status` fast on a large
 * tree. For each tracked file last found equal to its committed state, it
 * keeps the file's size and mtime and the index entry of the file's last
 * commit (its head). A file whose stat and head still match is not read.
 *
 * A cache under the cache contract: deleting it costs time only. It is the
 * one cache a reader writes: `status` refreshes it under a lock it only
 * tries (never waits for), writing a temp file and renaming it over.
 *
 * RACY ENTRIES. An entry is written only for a file whose mtime is older
 * than the second the run started, so an edit landing in the same second as
 * the check (same size, same whole-second mtime) is never taken for clean.
 *
 * Text, one entry per line: "<head> <size> <sec> <nsec> <path>", after a
 * "lapstat 1" line. A path holding a newline is never cached.
 */
#ifndef LAP_STATCACHE_H
#define LAP_STATCACHE_H

#include "platform.h"
#include "repo.h"

#define STATCACHE_NAME "statcache"

typedef struct {
    const char *path;
    int64_t head;
    PlatStat st;
} StatEntry;

typedef struct {
    Arena *a;
    StatEntry *v;
    size_t n, cap;
    int32_t *slots; /* open addressing into v; -1 is empty */
    size_t nslots;
} StatCache;

void statcache_init(StatCache *c, Arena *a);
/* Reads .lap/statcache; missing or unreadable leaves c empty. */
void statcache_load(StatCache *c, const Repo *r);
const StatEntry *statcache_get(const StatCache *c, const char *path);
/* Adds an entry; a path already present keeps its first entry. */
void statcache_add(StatCache *c, const char *path, int64_t head,
                   const PlatStat *st);
/* True when st can be trusted as clean from a run that started at
 * start_sec: its mtime is safely older than that second. */
bool statcache_settled(const PlatStat *st, int64_t start_sec);
/* True when a cached entry still describes the file: same head and stat. */
bool statcache_matches(const StatEntry *e, int64_t head, const PlatStat *st);
/* Writes c atomically (temp file, then rename). */
bool statcache_save(const StatCache *c, const Repo *r);

#endif /* LAP_STATCACHE_H */
