#include "test_tmp.h"

void tmp_dir(char *out, size_t outsz) {
    static int32_t seq = 0;
    const char *base = getenv("TMPDIR");
    if (!base || !base[0])
        base = "/tmp";
    char trimmed[KB_PATH_MAX];
    snprintf(trimmed, sizeof trimmed, "%s", base);
    size_t n = strlen(trimmed);
    while (n > 1 && trimmed[n - 1] == '/')
        trimmed[--n] = '\0';
    snprintf(out, outsz, "%s/kb-unit-%lld-%d", trimmed, (long long)plat_pid(),
             seq++);
    plat_mkdirs(out);
}

typedef struct {
    char *path;
    bool is_dir;
} Ent;

typedef struct {
    Arena *a;
    Ent *v;
    size_t n, cap;
} Found;

static WalkAction collect(const char *rel, bool is_dir, void *ud) {
    Found *f = (Found *)ud;
    ARENA_GROW(f->a, f->v, f->n, f->cap, Ent);
    f->v[f->n].path = arena_strdup(f->a, rel);
    f->v[f->n].is_dir = is_dir;
    f->n++;
    return WALK_CONT;
}

void tmp_rm(Arena *a, const char *root) {
    Found f;
    memset(&f, 0, sizeof f);
    f.a = a;
    plat_walk(a, root, collect, &f);
    char path[KB_PATH_MAX];
    for (size_t i = 0; i < f.n; i++) {
        if (f.v[i].is_dir)
            continue;
        snprintf(path, sizeof path, "%s/%s", root, f.v[i].path);
        plat_remove_file(path);
    }
    for (size_t i = f.n; i > 0; i--) {
        if (!f.v[i - 1].is_dir)
            continue;
        snprintf(path, sizeof path, "%s/%s", root, f.v[i - 1].path);
        plat_remove_dir(path);
    }
    plat_remove_dir(root);
}

int32_t tmp_count_files(Arena *a, const char *dir) {
    Found f;
    memset(&f, 0, sizeof f);
    f.a = a;
    plat_walk(a, dir, collect, &f);
    int32_t n = 0;
    for (size_t i = 0; i < f.n; i++) {
        if (!f.v[i].is_dir)
            n++;
    }
    return n;
}

bool tmp_append_raw(const char *path, const char *data, size_t len) {
    return plat_append_file_sync(path, data, len);
}
