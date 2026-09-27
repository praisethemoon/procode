#include "branches.h"

#include "json.h"
#include "platform.h"

static void path_of(const char *lapdir, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s", lapdir, LAP_BRANCHES_NAME);
}

void branches_add(Arena *a, Branches *b, BranchEntry e) {
    if (b->n == b->cap) {
        int32_t ncap = b->cap ? b->cap * 2 : 4;
        b->v = (BranchEntry *)arena_realloc(
            a, b->v, (size_t)b->cap * sizeof(BranchEntry),
            (size_t)ncap * sizeof(BranchEntry));
        b->cap = ncap;
    }
    b->v[b->n++] = e;
}

void branches_load(Arena *a, const char *lapdir, Branches *out) {
    memset(out, 0, sizeof *out);
    char path[LAP_PATH_MAX];
    path_of(lapdir, path, sizeof path);
    char *data;
    size_t len;
    if (!plat_is_file(path) || !plat_read_file(a, path, &data, &len))
        return;
    char err[128];
    JVal *v = json_parse(a, data, len, err, sizeof err);
    if (!v || v->t != J_ARR)
        return;
    for (size_t i = 0; i < v->arr.n; i++) {
        const JVal *o = v->arr.items[i];
        if (o->t != J_OBJ)
            continue;
        BranchEntry e;
        e.id = jobj_str(o, "id");
        e.name = jobj_str(o, "name");
        e.path = jobj_str(o, "path");
        e.base = jobj_str(o, "base");
        e.started = jobj_str(o, "started");
        if (e.id && e.name && e.path && e.base && e.started)
            branches_add(a, out, e);
    }
}

bool branches_save(Arena *a, const char *lapdir, const Branches *b) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_puts(&sb, "[");
    for (int32_t i = 0; i < b->n; i++) {
        const BranchEntry *e = &b->v[i];
        sb_puts(&sb, i ? ",\n " : "");
        sb_puts(&sb, "{\"id\":");
        json_escape_c(&sb, e->id);
        sb_puts(&sb, ",\"name\":");
        json_escape_c(&sb, e->name);
        sb_puts(&sb, ",\"path\":");
        json_escape_c(&sb, e->path);
        sb_puts(&sb, ",\"base\":");
        json_escape_c(&sb, e->base);
        sb_puts(&sb, ",\"started\":");
        json_escape_c(&sb, e->started);
        sb_puts(&sb, "}");
    }
    sb_puts(&sb, "]\n");
    size_t len = sb.len;
    char *data = sb_finish(&sb);
    char path[LAP_PATH_MAX];
    path_of(lapdir, path, sizeof path);
    return plat_write_file_atomic(path, data, len);
}

const BranchEntry *branches_find(const Branches *b, const char *key) {
    for (int32_t i = 0; i < b->n; i++) {
        if (strcmp(b->v[i].id, key) == 0 || strcmp(b->v[i].name, key) == 0)
            return &b->v[i];
    }
    return NULL;
}
