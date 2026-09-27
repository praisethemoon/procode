#include "branches.h"

#include "hist.h"
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

void branches_status(Arena *a, const char *lapdir, const RecLog *log,
                     const BranchEntry *e, BranchStatus *out) {
    memset(out, 0, sizeof *out);
    out->since_base = out->since_merge = -1;
    char elap[LAP_PATH_MAX], lineage[HIST_LINEAGE_MAX], err[512];
    snprintf(elap, sizeof elap, "%s/%s", e->path, LAP_DIR);
    out->present = plat_is_dir(elap) &&
                   hist_folder_lineage(a, elap, lineage, err, sizeof err) &&
                   strcmp(lineage, e->id) == 0;

    /* what merges of it adopted, and the files they stopped */
    StrSet seen;
    strset_init(&seen, a);
    int32_t cap = 0;
    for (int32_t i = 0; log && i < log->count; i++) {
        const Rec *m = &log->v[i];
        if (m->type != REC_MERGE || strcmp(m->branch, e->id) != 0)
            continue;
        out->merged = m->head;
        for (int32_t k = 0; k < m->stopped_n; k++) {
            if (!strset_add(&seen, m->stopped_file[k]))
                continue;
            if (out->nstopped == cap) {
                int32_t ncap = cap ? cap * 2 : 4;
                out->stopped = (const char **)arena_realloc(
                    a, out->stopped, (size_t)cap * sizeof(char *),
                    (size_t)ncap * sizeof(char *));
                out->stopped_at = (const char **)arena_realloc(
                    a, out->stopped_at, (size_t)cap * sizeof(char *),
                    (size_t)ncap * sizeof(char *));
                cap = ncap;
            }
            out->stopped_at[out->nstopped] = m->stopped_at[k];
            out->stopped[out->nstopped++] = m->stopped_file[k];
        }
    }

    /* its history: from its folder, else from its chunks here */
    Hist h;
    char *data;
    size_t len;
    RecLog blog;
    bool opened =
        out->present
            ? hist_open_folder(a, elap, &h, err, sizeof err)
            : hist_open_lineage(a, lapdir, e->id, &h, err, sizeof err);
    out->readable = opened && hist_read_all(a, &h, &data, &len) &&
                    rec_log_parse(a, data, len, NULL, NULL, &blog, err,
                                  sizeof err) &&
                    blog.count > 0;
    if (out->readable) {
        out->head = blog.v[blog.count - 1].hash;
        /* commits counted from the branch record, and from the merged
         * head; -1 until each is passed */
        int32_t since = -1, after = -1;
        for (int32_t i = 0; i < blog.count; i++) {
            const Rec *r = &blog.v[i];
            if (r->type == REC_BRANCH && strcmp(r->id, e->id) == 0) {
                since = 0;
            } else if (r->type == REC_COMMIT) {
                if (since >= 0)
                    since++;
                if (after >= 0)
                    after++;
            }
            if (out->merged && strcmp(r->hash, out->merged) == 0)
                after = 0;
        }
        out->since_base = since < 0 ? 0 : since;
        out->since_merge =
            out->merged ? (after < 0 ? 0 : after) : out->since_base;
    }

    bool whole = out->merged && out->head &&
                 strcmp(out->merged, out->head) == 0 && out->nstopped == 0;
    out->state = whole            ? "merged"
                 : !out->present  ? "missing"
                 : out->nstopped  ? "partly merged"
                                  : "active";
}

const BranchEntry *branches_find(const Branches *b, const char *key) {
    for (int32_t i = 0; i < b->n; i++) {
        if (strcmp(b->v[i].id, key) == 0 || strcmp(b->v[i].name, key) == 0)
            return &b->v[i];
    }
    return NULL;
}
