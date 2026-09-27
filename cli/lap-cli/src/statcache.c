#include "statcache.h"

static const char HEADER[] = "lapstat 1\n";

static void rehash(StatCache *c, size_t nslots) {
    c->slots = (int32_t *)arena_alloc(c->a, nslots * sizeof(int32_t));
    for (size_t i = 0; i < nslots; i++)
        c->slots[i] = -1;
    c->nslots = nslots;
    for (size_t i = 0; i < c->n; i++) {
        size_t h = (size_t)str_hash(str_c(c->v[i].path)) & (nslots - 1);
        while (c->slots[h] >= 0)
            h = (h + 1) & (nslots - 1);
        c->slots[h] = (int32_t)i;
    }
}

void statcache_init(StatCache *c, Arena *a) {
    memset(c, 0, sizeof *c);
    c->a = a;
    rehash(c, 64);
}

const StatEntry *statcache_get(const StatCache *c, const char *path) {
    size_t h = (size_t)str_hash(str_c(path)) & (c->nslots - 1);
    while (c->slots[h] >= 0) {
        const StatEntry *e = &c->v[c->slots[h]];
        if (strcmp(e->path, path) == 0)
            return e;
        h = (h + 1) & (c->nslots - 1);
    }
    return NULL;
}

void statcache_add(StatCache *c, const char *path, int64_t head,
                   const PlatStat *st) {
    if (strchr(path, '\n') || statcache_get(c, path))
        return;
    ARENA_GROW(c->a, c->v, c->n, c->cap, StatEntry);
    c->v[c->n].path = arena_strdup(c->a, path);
    c->v[c->n].head = head;
    c->v[c->n].st = *st;
    c->n++;
    if (c->n * 2 > c->nslots)
        rehash(c, c->nslots * 2);
    else {
        size_t h = (size_t)str_hash(str_c(path)) & (c->nslots - 1);
        while (c->slots[h] >= 0)
            h = (h + 1) & (c->nslots - 1);
        c->slots[h] = (int32_t)(c->n - 1);
    }
}

bool statcache_settled(const PlatStat *st, int64_t start_sec) {
    return st->mtime_sec < start_sec;
}

bool statcache_matches(const StatEntry *e, int64_t head, const PlatStat *st) {
    return e->head == head && e->st.size == st->size &&
           e->st.mtime_sec == st->mtime_sec &&
           e->st.mtime_nsec == st->mtime_nsec;
}

static void cache_path(const Repo *r, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", r->lapdir, STATCACHE_NAME);
}

void statcache_load(StatCache *c, const Repo *r) {
    char path[LAP_PATH_MAX];
    cache_path(r, path, sizeof path);
    char *data = NULL;
    size_t len = 0;
    if (!plat_read_file(c->a, path, &data, &len) ||
        strncmp(data, HEADER, sizeof HEADER - 1) != 0)
        return;
    char *p = data + sizeof HEADER - 1;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            break; /* a torn last line is dropped, like the log's */
        *nl = '\0';
        char *end;
        long long head = strtoll(p, &end, 10);
        unsigned long long size = strtoull(end, &end, 10);
        long long sec = strtoll(end, &end, 10);
        long nsec = strtol(end, &end, 10);
        if (*end == ' ' && end[1]) {
            PlatStat st = {(uint64_t)size, (int64_t)sec, (int32_t)nsec};
            statcache_add(c, end + 1, (int64_t)head, &st);
        }
        p = nl + 1;
    }
}

bool statcache_save(const StatCache *c, const Repo *r) {
    StrBuf sb;
    sb_init(&sb, c->a);
    sb_puts(&sb, HEADER);
    for (size_t i = 0; i < c->n; i++) {
        const StatEntry *e = &c->v[i];
        sb_printf(&sb, "%lld %llu %lld %d %s\n", (long long)e->head,
                  (unsigned long long)e->st.size, (long long)e->st.mtime_sec,
                  (int)e->st.mtime_nsec, e->path);
    }
    char path[LAP_PATH_MAX];
    cache_path(r, path, sizeof path);
    size_t len = sb.len;
    return plat_write_file_atomic(path, sb_finish(&sb), len);
}
