/* Generated merge cases: fixed seeds, each one a real git worktree, real
 * lap commits on both sides, a real git merge and lap merge, and the
 * invariants every merge must keep checked after each one.
 *
 *   lap-prop [first last]   run seeds first..last (default 1..60)
 *   LAP_PROP_SEED=<n>       replay seed n alone
 *   LAP_PROP_TRACE=1        print every command and its output
 *   LAP_PROP_JOBS=<n>       cases run side by side (default: the CPUs, 8 at
 *                           most)
 *   LAP=<path>              the lap under test (default bin/lap)
 *
 * A seed is the whole case: the driver's own PRNG (splitmix64, never the
 * platform's rand()) picks the files, the edits, the sessions and the merge
 * order from it, and nothing is taken from the clock, so a seed is the same
 * case on every run and every machine. A third of the seeds nest (b1 from
 * main, b2 from b1) and merge in one of four orders; a quarter seal chunks
 * mid-case (a small LAP_TEST_CHUNK_BYTES); every tenth also cuts its merge
 * into main after each record and runs it again.
 *
 * After every merge: no git conflict under .lap/; --dry-run changes no byte
 * and reports what the merge then does; every file lap did not stop, where
 * git merged cleanly, is recorded as git merged it; lap verify --deep
 * passes; no original record reaches the history twice; and a cut merge,
 * run again, ends with the whole run's records, ids and shadows.
 *
 * Every git and lap runs with HOME and TMPDIR inside the driver's work
 * folder (made with mkdtemp under $TMPDIR, else /tmp). A passing case's
 * folder is removed; a failing one is kept and its seed and path printed.
 */
#define _DEFAULT_SOURCE /* popen, mkdtemp, realpath, scandir under -std=c11 */
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sha256.h"

#define PATHSZ 1024
#define MAXLINES 600

/* ---- the seed's randomness --------------------------------------------- */

static uint64_t rng_state;

static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
/* 0..n-1 */
static int pick(int n) { return (int)(rng_next() % (uint64_t)n); }
static bool chance(int percent) { return pick(100) < percent; }

/* ---- running git and lap ----------------------------------------------- */

static char lap_bin[PATHSZ];
static char work_root[PATHSZ];
static bool trace;
static char out[1 << 20]; /* the last command's stdout and stderr */
static char why[4096];    /* the current case's failure */

static bool fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    return false;
}
#define TRY(x)                                                                 \
    do {                                                                       \
        if (!(x))                                                              \
            return false;                                                      \
    } while (0)

/* Runs a shell command in dir, its output in out; the exit status, or -1. */
static int sh(const char *dir, const char *fmt, ...) {
    char cmd[8192];
    int n = snprintf(cmd, sizeof cmd, "cd '%s' && { ", dir);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(cmd + n, sizeof cmd - (size_t)n, fmt, ap);
    va_end(ap);
    snprintf(cmd + n, sizeof cmd - (size_t)n, "; } 2>&1");
    FILE *p = popen(cmd, "r");
    size_t len = 0;
    if (p) {
        size_t got;
        while ((got = fread(out + len, 1, sizeof out - 1 - len, p)) > 0)
            len += got;
    }
    out[len] = '\0';
    int st = p ? pclose(p) : -1;
    int rc = st != -1 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (trace)
        fprintf(stderr, "$ %s\n[%d] %s", cmd, rc, out);
    return rc;
}

/* ---- files -------------------------------------------------------------- */

typedef struct {
    char *v[MAXLINES];
    int n;
} Lines;

static void lines_free(Lines *l) {
    for (int i = 0; i < l->n; i++)
        free(l->v[i]);
    l->n = 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 4096, n = 0, got;
    char *d = malloc(cap);
    while ((got = fread(d + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (cap - n - 1 == 0)
            d = realloc(d, cap *= 2);
    }
    fclose(f);
    d[n] = '\0';
    if (len)
        *len = n;
    return d;
}

static bool write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return fail("cannot write %s", path);
    fwrite(data, 1, len, f);
    fclose(f);
    return true;
}

static bool lines_read(const char *path, Lines *l) {
    size_t len;
    char *d = read_file(path, &len);
    if (!d)
        return fail("cannot read %s", path);
    l->n = 0;
    for (char *s = d; *s && l->n < MAXLINES;) {
        char *e = strchr(s, '\n');
        size_t k = e ? (size_t)(e - s) : strlen(s);
        l->v[l->n] = malloc(k + 1);
        memcpy(l->v[l->n], s, k);
        l->v[l->n++][k] = '\0';
        s += k + (e ? 1 : 0);
    }
    free(d);
    return true;
}

static bool lines_write(const char *path, const Lines *l) {
    size_t cap = 1, n = 0;
    for (int i = 0; i < l->n; i++)
        cap += strlen(l->v[i]) + 1;
    char *d = malloc(cap);
    for (int i = 0; i < l->n; i++) {
        size_t k = strlen(l->v[i]);
        memcpy(d + n, l->v[i], k);
        n += k;
        d[n++] = '\n';
    }
    bool ok = write_file(path, d, n);
    free(d);
    return ok;
}

/* Removes a folder the driver made, and everything in it. */
static void remove_tree(const char *path) {
    if (strncmp(path, work_root, strlen(work_root)) != 0)
        return; /* never anything outside the work folder */
    struct stat st;
    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char sub[PATHSZ];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            remove_tree(sub);
        }
        if (d)
            closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static int name_cmp(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

/* Every file under dir, its path and bytes, in name order; .git left out
 * at the top. */
static void hash_tree(Sha256 *c, const char *dir, const char *rel) {
    char path[PATHSZ];
    snprintf(path, sizeof path, "%s%s%s", dir, rel[0] ? "/" : "", rel);
    struct dirent **ents;
    int n = scandir(path, &ents, NULL, name_cmp);
    for (int i = 0; i < n; i++) {
        const char *nm = ents[i]->d_name;
        char sub[PATHSZ], full[PATHSZ];
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", nm);
        snprintf(full, sizeof full, "%s/%s", dir, sub);
        struct stat st;
        if (!strcmp(nm, ".") || !strcmp(nm, "..") ||
            (!rel[0] && !strcmp(nm, ".git")) || lstat(full, &st) != 0) {
            free(ents[i]);
            continue;
        }
        sha256_update(c, sub, strlen(sub) + 1);
        if (S_ISDIR(st.st_mode)) {
            hash_tree(c, dir, sub);
        } else {
            size_t len;
            char *d = read_file(full, &len);
            if (d)
                sha256_update(c, d, len);
            free(d);
        }
        free(ents[i]);
    }
    if (n >= 0)
        free(ents);
}

static void tree_digest(const char *dir, char hex[65]) {
    Sha256 c;
    uint8_t d[32];
    sha256_init(&c);
    hash_tree(&c, dir, "");
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

/* ---- a folder: the parent or a branch ---------------------------------- */

typedef struct {
    char dir[PATHSZ];
    const char *branch; /* "main", "b1", "b2" */
    bool session;       /* a session of its own is open */
    char last[32];      /* its last commit's id, for an amendment */
    int n;              /* counts its edits, for unique text */
} Side;

static int commits, merges, reruns, stops_git_clean;

/* lap commits every edit pending in file, one --edit 1 at a time. */
static bool commit_file(Side *s, const char *file, const char *what) {
    for (int k = 1; k <= 30; k++) {
        int rc = sh(s->dir,
                    "'%s' commit %s --branch %s%s --edit 1 --force-message "
                    "-i 'generated work on %s' -b '%s: %s, part %d'",
                    lap_bin, file, s->branch,
                    s->session ? "" : " --no-session", s->branch, s->branch,
                    what, k);
        if (rc == 0) {
            commits++;
            const char *b = strchr(out, '[');
            if (b && sscanf(b + 1, "%31[^ ]", s->last) != 1)
                s->last[0] = '\0';
            continue;
        }
        if (strstr(out, "no changes"))
            return true;
        return fail("lap commit in %s: %s", s->dir, out);
    }
    return fail("lap commit in %s: more than 30 edits in %s", s->dir, file);
}

static void new_text(char *buf, size_t sz, Side *s, int blank_percent) {
    int r = pick(100);
    if (r < blank_percent)
        buf[0] = '\0';
    else if (r < blank_percent + 12)
        snprintf(buf, sz, "dup");
    else
        snprintf(buf, sz, "%s e%d", s->branch, ++s->n);
}

/* An edit: at, how many lines it removes, and the lines it puts there. */
typedef struct {
    int at, del, add;
    char text[4][64];
} Edit;

static void make_edit(Edit *e, const Lines *l, Side *s) {
    int op = l->n > 1 ? pick(3) : 0; /* insert, delete, replace */
    int r = pick(10);
    e->at = r < 2 ? 0 : r < 4 ? l->n : pick(l->n + 1);
    e->del = e->add = 0;
    if (op != 0 && e->at == l->n)
        e->at = l->n - 1;
    if (op != 0) {
        e->del = 1 + pick(3);
        if (e->at + e->del > l->n)
            e->del = l->n - e->at;
    }
    if (op != 1) {
        e->add = 1 + pick(3);
        for (int i = 0; i < e->add; i++)
            new_text(e->text[i], sizeof e->text[i], s, 12);
    }
}

static void apply_edit(Lines *l, const Edit *e) {
    for (int i = 0; i < e->del; i++)
        free(l->v[e->at + i]);
    memmove(&l->v[e->at + e->add], &l->v[e->at + e->del],
            (size_t)(l->n - e->at - e->del) * sizeof(char *));
    l->n += e->add - e->del;
    for (int i = 0; i < e->add; i++)
        l->v[e->at + i] = strdup(e->text[i]);
}

static bool edit_file(Side *s, const char *file, const Edit *e) {
    char path[PATHSZ];
    snprintf(path, sizeof path, "%s/%s", s->dir, file);
    Lines l;
    TRY(lines_read(path, &l));
    if (l.n + 4 >= MAXLINES) {
        lines_free(&l);
        return true;
    }
    apply_edit(&l, e);
    bool ok = lines_write(path, &l);
    lines_free(&l);
    return ok;
}

/* The case's files: a stopped file stays stopped, so work spread over
 * several keeps later merges placing. */
static const char *files[] = {"f.txt", "g.txt", "h.txt", "k.txt"};
#define NFILES 4

static const char *some_file(void) {
    int r = pick(10);
    return files[r < 4 ? 0 : r < 6 ? 1 : r < 8 ? 2 : 3];
}

static bool random_edit(Side *s, const char *file, Edit *e) {
    char path[PATHSZ];
    snprintf(path, sizeof path, "%s/%s", s->dir, file);
    Lines l;
    TRY(lines_read(path, &l));
    make_edit(e, &l, s);
    lines_free(&l);
    return edit_file(s, file, e);
}

/* n steps of work in one folder: edits, a change in two steps, a change
 * undone, an amendment, sessions opened and closed. */
static bool work(Side *s, int n) {
    for (int i = 0; i < n; i++) {
        const char *file = some_file();
        char what[96];
        Edit e;
        int r = pick(100);
        if (r < 55) {
            TRY(random_edit(s, file, &e));
            snprintf(what, sizeof what, "edit %d of %s", s->n, file);
            TRY(commit_file(s, file, what));
        } else if (r < 70) { /* a change made in two steps */
            TRY(random_edit(s, file, &e));
            snprintf(what, sizeof what, "first step %d in %s", s->n, file);
            TRY(commit_file(s, file, what));
            if (e.add > 0) {
                Edit f = {e.at, e.add, e.add, {{0}}};
                for (int k = 0; k < e.add; k++)
                    snprintf(f.text[k], sizeof f.text[k], "%s e%d",
                             s->branch, ++s->n);
                TRY(edit_file(s, file, &f));
                snprintf(what, sizeof what, "second step %d in %s", s->n,
                         file);
                TRY(commit_file(s, file, what));
            }
        } else if (r < 82) { /* a change, then undone */
            char path[PATHSZ];
            snprintf(path, sizeof path, "%s/%s", s->dir, file);
            size_t len;
            char *before = read_file(path, &len);
            if (!before)
                return fail("cannot read %s", path);
            TRY(random_edit(s, file, &e));
            snprintf(what, sizeof what, "edit %d of %s, to undo", s->n,
                     file);
            TRY(commit_file(s, file, what));
            bool ok = write_file(path, before, len);
            free(before);
            TRY(ok);
            snprintf(what, sizeof what, "undoes edit %d of %s", s->n, file);
            TRY(commit_file(s, file, what));
        } else if (r < 90 && s->last[0]) {
            if (sh(s->dir,
                   "'%s' amend %s --branch %s --force-message -i 'generated "
                   "amendment on %s' -b 'amends %s in %s, amendment %d'",
                   lap_bin, s->last, s->branch, s->branch, s->last,
                   s->branch, ++s->n) != 0)
                return fail("lap amend in %s: %s", s->dir, out);
        } else if (s->session) {
            if (sh(s->dir, "'%s' session end --branch %s", lap_bin,
                   s->branch) != 0)
                return fail("lap session end in %s: %s", s->dir, out);
            s->session = false;
        } else {
            if (sh(s->dir,
                   "'%s' session start 'generated session %d on %s' "
                   "--branch %s --meta seed=%d",
                   lap_bin, s->n, s->branch, s->branch, i) != 0)
                return fail("lap session start in %s: %s", s->dir, out);
            s->session = true;
        }
    }
    return true;
}

/* The same edit, made and committed in two folders whose file is the same. */
static bool same_change(Side *a, Side *b) {
    const char *file = some_file();
    Edit e;
    TRY(random_edit(a, file, &e));
    TRY(edit_file(b, file, &e));
    TRY(commit_file(a, file, "the change both sides make"));
    return commit_file(b, file, "the change both sides make");
}

static bool git_commit(Side *s, const char *msg) {
    if (sh(s->dir, "git add -A && { git diff --cached --quiet || "
                   "git commit -qm '%s'; }",
           msg) != 0)
        return fail("git commit in %s: %s", s->dir, out);
    return true;
}

/* ---- the case's folders -------------------------------------------------- */

static char case_dir[PATHSZ];

static bool make_parent(Side *p) {
    snprintf(p->dir, sizeof p->dir, "%s/p", case_dir);
    p->branch = "main";
    mkdir(p->dir, 0755);
    if (sh(p->dir, "git init -q . && printf '.lap/*\\n!.lap/log/\\n' > "
                   ".gitignore && '%s' init",
           lap_bin) != 0)
        return fail("git init / lap init: %s", out);
    for (int f = 0; f < NFILES; f++) {
        Lines l = {{0}, 0};
        int n = f == 0 ? 8 + pick(30) : 2 + pick(15);
        for (int i = 0; i < n; i++) {
            char t[64];
            int r = pick(100);
            if (r < 10)
                t[0] = '\0';
            else if (r < 20)
                snprintf(t, sizeof t, "dup");
            else
                snprintf(t, sizeof t, "base %c%d", files[f][0], i + 1);
            l.v[l.n++] = strdup(t);
        }
        char path[PATHSZ];
        snprintf(path, sizeof path, "%s/%s", p->dir, files[f]);
        bool ok = lines_write(path, &l);
        lines_free(&l);
        TRY(ok);
    }
    if (sh(p->dir,
           "for f in f.txt g.txt h.txt k.txt .lapignore .gitignore; do "
           "'%s' commit $f --no-session -i 'seed the generated case' "
           "-b \"records $f as the base\" || exit 1; done",
           lap_bin) != 0)
        return fail("base commits: %s", out);
    return git_commit(p, "base");
}

static bool start_branch(Side *from, Side *b, const char *name) {
    snprintf(b->dir, sizeof b->dir, "%s/%s", case_dir, name);
    b->branch = name;
    if (sh(from->dir, "git worktree add -q '%s' -b %s", b->dir, name) != 0)
        return fail("git worktree add %s: %s", name, out);
    if (sh(b->dir, "'%s' branch start %s --from '%s'", lap_bin, name,
           from->dir) != 0)
        return fail("lap branch start %s: %s", name, out);
    return git_commit(from, "sealed for a branch");
}

/* ---- the invariants ---------------------------------------------------- */

/* The value of "key":"..." in a record line, into v. */
static bool field(const char *line, const char *key, char *v, size_t vsz) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p)
        return false;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= vsz)
        return false;
    memcpy(v, p, (size_t)(e - p));
    v[e - p] = '\0';
    return true;
}

static void folder_lineage(const Side *s, char lin[32]) {
    char path[PATHSZ];
    snprintf(path, sizeof path, "%s/.lap/lineage", s->dir);
    char *d = read_file(path, NULL);
    snprintf(lin, 32, "%s", d ? d : "main");
    lin[strcspn(lin, "\r\n")] = '\0';
    free(d);
}

typedef struct {
    char hash[65], from[65];
    bool own;
} Link;

/* No original record reaches this folder's lineage twice: following each
 * adopted record's from to the record it was first made as, no two meet. */
static bool from_once(const Side *s) {
    char lin[32], logdir[PATHSZ];
    folder_lineage(s, lin);
    snprintf(logdir, sizeof logdir, "%s/.lap/log", s->dir);
    struct dirent **ents;
    int ne = scandir(logdir, &ents, NULL, name_cmp);
    size_t cap = 256, n = 0;
    Link *v = malloc(cap * sizeof *v);
    for (int i = 0; i < ne; i++) {
        const char *nm = ents[i]->d_name;
        bool own = !strncmp(nm, lin, strlen(lin)) && nm[strlen(lin)] == '.';
        char path[PATHSZ];
        snprintf(path, sizeof path, "%s/%s", logdir, nm);
        char *d = strstr(nm, ".jsonl") ? read_file(path, NULL) : NULL;
        for (char *line = d; line && *line;) {
            char *e = strchr(line, '\n');
            if (e)
                *e = '\0';
            if (n == cap)
                v = realloc(v, (cap *= 2) * sizeof *v);
            if (field(line, "hash", v[n].hash, 65)) {
                if (!field(line, "from", v[n].from, 65))
                    v[n].from[0] = '\0';
                v[n++].own = own;
            }
            line = e ? e + 1 : NULL;
        }
        free(d);
        free(ents[i]);
    }
    if (ne >= 0)
        free(ents);
    char (*orig)[65] = malloc((n + 1) * sizeof *orig);
    size_t no = 0;
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        if (!v[i].own || !v[i].from[0])
            continue;
        char root[65];
        snprintf(root, sizeof root, "%s", v[i].from);
        for (int hop = 0; hop < 16; hop++) {
            size_t j = 0;
            while (j < n && strcmp(v[j].hash, root))
                j++;
            if (j == n || !v[j].from[0])
                break;
            snprintf(root, sizeof root, "%s", v[j].from);
        }
        for (size_t k = 0; k < no; k++)
            if (!strcmp(orig[k], root))
                ok = fail("record %.12s reached %s's history twice (the "
                          "second as %.12s)",
                          root, s->branch, v[i].hash);
        snprintf(orig[no++], 65, "%s", root);
    }
    free(orig);
    free(v);
    return ok;
}

/* The folder's own chunks, with what changes between two runs of one merge
 * (the time, and the hashes it goes into) cut out. */
static char *history_text(const Side *s) {
    char lin[32], logdir[PATHSZ];
    folder_lineage(s, lin);
    snprintf(logdir, sizeof logdir, "%s/.lap/log", s->dir);
    struct dirent **ents;
    int ne = scandir(logdir, &ents, NULL, name_cmp);
    size_t cap = 1 << 16, n = 0;
    char *t = malloc(cap);
    t[0] = '\0';
    for (int i = 0; i < ne; i++) {
        const char *nm = ents[i]->d_name;
        if (!strncmp(nm, lin, strlen(lin)) && nm[strlen(lin)] == '.') {
            char path[PATHSZ];
            snprintf(path, sizeof path, "%s/%s", logdir, nm);
            size_t len;
            char *d = read_file(path, &len);
            for (char *p = d; p && *p; p++) {
                static const char *cut[] = {"\"ts\":\"", "\"prev\":\"",
                                            "\"hash\":\""};
                for (int c = 0; c < 3; c++)
                    if (!strncmp(p, cut[c], strlen(cut[c]))) {
                        p = strchr(p + strlen(cut[c]), '"');
                        break;
                    }
                if (n + 2 >= cap)
                    t = realloc(t, cap *= 2);
                t[n++] = *p;
            }
            free(d);
        }
        free(ents[i]);
    }
    if (ne >= 0)
        free(ents);
    t[n] = '\0';
    return t;
}

/* What a merge report says, less what only a real run has (its ids) and
 * the dry_run flag itself. */
static char *report_core(const char *json) {
    const char *b = strstr(json, "\"branch\":");
    const char *e = b ? strstr(b, ",\"commits\":") : NULL;
    if (!e)
        return NULL;
    char *r = malloc((size_t)(e - b) + 1);
    memcpy(r, b, (size_t)(e - b));
    r[e - b] = '\0';
    return r;
}

static bool settle(Side *s) {
    /* what the merge left: committed as the parent's own work */
    for (int round = 0; round < 3; round++) {
        sh(s->dir, "'%s' status", lap_bin);
        if (strstr(out, "clean: working tree matches the last commit"))
            return true;
        for (int f = 0; f < NFILES; f++)
            TRY(commit_file(s, files[f], "takes what the merge left"));
    }
    return fail("%s is not clean after committing what the merge left: %s",
                s->dir, out);
}

/* Reruns the merge cut after each record, from a copy of the folder as the
 * git merge left it, and requires each rerun to end where the whole run did. */
static bool crash_loop(Side *into, const char *copy, const char *branch) {
    char *want = history_text(into);
    char shadow[PATHSZ], want_shadow[65], got_shadow[65];
    snprintf(shadow, sizeof shadow, "%s/.lap/shadow", into->dir);
    tree_digest(shadow, want_shadow);
    bool ok = true;
    for (int n = 0; ok; n++) {
        Side c = *into;
        snprintf(c.dir, sizeof c.dir, "%s/crash", case_dir);
        if (sh(case_dir, "cp -R '%s' '%s'", copy, c.dir) != 0) {
            ok = fail("cannot copy %s: %s", copy, out);
            break;
        }
        char env[64];
        snprintf(env, sizeof env, "%d", n);
        setenv("LAP_TEST_MERGE_FAIL_AFTER", env, 1);
        int rc = sh(c.dir, "'%s' merge %s", lap_bin, branch);
        unsetenv("LAP_TEST_MERGE_FAIL_AFTER");
        bool whole = rc == 0;
        if (!whole && !strstr(out, "LAP_TEST_MERGE_FAIL_AFTER"))
            ok = fail("the merge cut after %d records failed otherwise: %s",
                      n, out);
        else if (!whole && sh(c.dir, "'%s' merge %s", lap_bin, branch) != 0)
            ok = fail("the rerun after a cut at %d failed: %s", n, out);
        if (ok) {
            char *got = history_text(&c);
            snprintf(shadow, sizeof shadow, "%s/.lap/shadow", c.dir);
            tree_digest(shadow, got_shadow);
            if (strcmp(got, want))
                ok = fail("cut after %d records, the rerun's history differs "
                          "from the whole run's (kept in %s)",
                          n, c.dir);
            else if (strcmp(got_shadow, want_shadow))
                ok = fail("cut after %d records, the rerun's shadows differ "
                          "(kept in %s)",
                          n, c.dir);
            free(got);
            reruns++;
        }
        if (!ok)
            break;
        remove_tree(c.dir);
        if (whole)
            break;
        if (n > 500) {
            ok = fail("the merge never finished uncut");
            break;
        }
    }
    free(want);
    return ok;
}

/* git merge, then lap merge, of branch into a folder, checking every
 * invariant; what is left is committed after. */
static bool merge(Side *into, const char *branch, Side **all, int nall,
                  bool crash) {
    for (int i = 0; i < nall; i++)
        TRY(git_commit(all[i], "work"));
    merges++;
    int grc = sh(into->dir, "git merge -q --no-edit %s", branch);
    bool git_clean = grc == 0;
    if (!git_clean) {
        if (sh(into->dir, "git diff --name-only --diff-filter=U") != 0 ||
            !out[0])
            return fail("git merge %s into %s failed: %s", branch,
                        into->branch, out);
        char list[4096];
        snprintf(list, sizeof list, "%s", out);
        for (char *p = strtok(list, "\n"); p; p = strtok(NULL, "\n")) {
            if (!strncmp(p, ".lap/", 5))
                return fail("git conflicts in %s merging %s into %s", p,
                            branch, into->branch);
            if (sh(into->dir, "git checkout --%s -- '%s' && git add '%s'",
                   chance(50) ? "ours" : "theirs", p, p) != 0)
                return fail("resolving %s: %s", p, out);
        }
        if (sh(into->dir, "git commit -qm resolved") != 0)
            return fail("git commit of the resolution: %s", out);
    }

    char before[65], after[65];
    tree_digest(into->dir, before);
    if (sh(into->dir, "'%s' merge %s --dry-run --json", lap_bin, branch) != 0)
        return fail("lap merge %s --dry-run into %s: %s", branch,
                    into->branch, out);
    char *dry = report_core(out);
    tree_digest(into->dir, after);
    if (strcmp(before, after)) {
        free(dry);
        return fail("lap merge %s --dry-run changed %s", branch, into->dir);
    }
    char copy[PATHSZ];
    snprintf(copy, sizeof copy, "%s/before-merge", case_dir);
    if (crash && sh(case_dir, "cp -R '%s' '%s'", into->dir, copy) != 0) {
        free(dry);
        return fail("cannot copy %s: %s", into->dir, out);
    }
    int rc = sh(into->dir, "'%s' merge %s --json", lap_bin, branch);
    char *real = report_core(out);
    bool ok = true;
    if (rc != 0 || !real || !dry)
        ok = fail("lap merge %s into %s: %s", branch, into->branch, out);
    else if (strcmp(dry, real))
        ok = fail("lap merge %s into %s did not do what --dry-run said:\n"
                  "  dry run: %s\n  merge:   %s",
                  branch, into->branch, dry, real);
    bool stopped = real && !strstr(real, "\"stopped\":[]");
    /* A change both sides made is no reference: git merges the two from a
     * base before both, and among repeated or blank lines its alignment
     * can keep both (a blank line inserted twice), where lap records it
     * once. */
    bool twice = real && !strstr(real, "\"already\":[]");
    /* the files it stopped, each as "\n<file>\n" */
    char stops[4096] = "\n";
    for (const char *p = real; p && (p = strstr(p, "\"file\":\"")); p++) {
        size_t n = strlen(stops);
        const char *f = p + 8, *e = strchr(f, '"');
        if (e && n + (size_t)(e - f) + 2 < sizeof stops)
            snprintf(stops + n, sizeof stops - n, "%.*s\n", (int)(e - f), f);
    }
    free(dry);
    free(real);
    TRY(ok);
    if (git_clean && stopped)
        stops_git_clean++;
    if (git_clean && !twice) {
        /* every file lap did not stop is recorded as git merged it */
        sh(into->dir, "'%s' status", lap_bin);
        for (const char *p = out; (p = strstr(p, "modified  ")); p++) {
            char file[256], key[260];
            if (sscanf(p + 10, "%255s", file) != 1)
                continue;
            snprintf(key, sizeof key, "\n%s\n", file);
            if (!strstr(stops, key))
                return fail("git merged %s into %s cleanly and lap did not "
                            "stop %s, but it is not recorded as git merged "
                            "it:\n%s",
                            branch, into->branch, file, out);
        }
    }
    if (sh(into->dir, "'%s' verify --deep", lap_bin) != 0 ||
        !strstr(out, " 0 mismatch"))
        return fail("lap verify --deep after merging %s into %s:\n%s",
                    branch, into->branch, out);
    TRY(from_once(into));
    if (crash) {
        TRY(crash_loop(into, copy, branch));
        remove_tree(copy);
    }
    TRY(settle(into));
    return git_commit(into, "lap merge");
}

/* ---- a case ------------------------------------------------------------- */

static const char *case_kind;

/* A parent and one branch, merged once or more. */
static bool flat_case(bool crash) {
    Side p = {0}, b = {0};
    TRY(make_parent(&p));
    TRY(start_branch(&p, &b, "b"));
    Side *all[] = {&p, &b};
    int rounds = 1 + pick(3);
    for (int r = 0; r < rounds; r++) {
        if (r == 0 && chance(40))
            TRY(same_change(&p, &b));
        TRY(work(&b, 1 + pick(5)));
        TRY(work(&p, pick(4)));
        TRY(merge(&p, "b", all, 2, crash && r == 0));
    }
    return true;
}

/* b1 from main, b2 from b1, merged in one of the four orders. */
static bool nested_case(int order, bool crash) {
    Side p = {0}, b1 = {0}, b2 = {0};
    TRY(make_parent(&p));
    TRY(start_branch(&p, &b1, "b1"));
    if (chance(40))
        TRY(same_change(&p, &b1));
    TRY(work(&b1, 1 + pick(3)));
    TRY(git_commit(&b1, "b1 before b2"));
    TRY(start_branch(&b1, &b2, "b2"));
    if (chance(40))
        TRY(same_change(&b1, &b2));
    TRY(work(&b2, 1 + pick(4)));
    TRY(work(&b1, pick(3)));
    TRY(work(&p, pick(3)));
    Side *all[] = {&p, &b1, &b2};
    switch (order) {
    case 0: /* b2 -> b1 -> main */
        TRY(merge(&b1, "b2", all, 3, false));
        TRY(merge(&p, "b1", all, 3, crash));
        break;
    case 1: /* b2 -> main, then b1 */
        TRY(merge(&p, "b2", all, 3, crash));
        TRY(merge(&p, "b1", all, 3, false));
        break;
    case 2: /* b2 -> b1, b2 -> main, then b1 */
        TRY(merge(&b1, "b2", all, 3, false));
        TRY(merge(&p, "b2", all, 3, crash));
        TRY(merge(&p, "b1", all, 3, false));
        break;
    default: /* b2 -> b1 -> main, then b2 -> main */
        TRY(merge(&b1, "b2", all, 3, false));
        TRY(merge(&p, "b1", all, 3, crash));
        TRY(merge(&p, "b2", all, 3, false));
        break;
    }
    return true;
}

static const char *orders[] = {"b2->b1->main", "b2->main, b1",
                               "b2->b1, b2->main, b1",
                               "b2->b1->main, b2->main"};

static bool run_case(int seed) {
    rng_state = (uint64_t)seed * 0xD1B54A32D192ED03ull;
    bool nested = seed % 3 == 0, crash = seed % 10 == 0;
    if (seed % 4 == 1) {
        char lim[32];
        snprintf(lim, sizeof lim, "%d", 600 + pick(1500));
        setenv("LAP_TEST_CHUNK_BYTES", lim, 1);
    } else {
        unsetenv("LAP_TEST_CHUNK_BYTES");
    }
    int order = (seed / 3) % 4;
    case_kind = nested ? orders[order] : "flat";
    mkdir(case_dir, 0755);
    return nested ? nested_case(order, crash) : flat_case(crash);
}

/* Runs one case and leaves its verdict in <work>/seed-<n>.result: ok, the
 * counts, then the kind and why. Cases share nothing, so they run side by
 * side, each in a child process; the verdicts are read in seed order. */
static void run_child_here(int seed) {
    snprintf(case_dir, sizeof case_dir, "%s/seed-%d", work_root, seed);
    merges = commits = reruns = stops_git_clean = 0;
    bool ok = run_case(seed);
    if (ok)
        remove_tree(case_dir);
    char path[PATHSZ];
    snprintf(path, sizeof path, "%s/seed-%d.result", work_root, seed);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d %d %d %d %d\n(%s): %s", ok, merges, commits, reruns,
                stops_git_clean, case_kind, ok ? "" : why);
        fclose(f);
    }
}

static void run_child(int seed) {
    run_child_here(seed);
    fflush(NULL);
    _exit(0);
}

int main(int argc, char **argv) {
    int first = 1, last = 60;
    if (argc == 3) {
        first = atoi(argv[1]);
        last = atoi(argv[2]);
    }
    const char *one = getenv("LAP_PROP_SEED");
    if (one && *one)
        first = last = atoi(one);
    trace = getenv("LAP_PROP_TRACE") && *getenv("LAP_PROP_TRACE");
    const char *lap = getenv("LAP");
    if (!realpath(lap && *lap ? lap : "bin/lap", lap_bin)) {
        fprintf(stderr, "lap-prop: no lap binary at %s (set LAP)\n",
                lap && *lap ? lap : "bin/lap");
        return 1;
    }
    if (system("git --version >/dev/null 2>&1") != 0) {
        printf("lap-prop: skipped: git is not installed\n");
        return 0;
    }
    const char *tmp = getenv("TMPDIR");
    snprintf(work_root, sizeof work_root, "%s/lap-prop.XXXXXX",
             tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(work_root)) {
        fprintf(stderr, "lap-prop: cannot make a folder in %s\n",
                tmp && *tmp ? tmp : "/tmp");
        return 1;
    }
    /* git and lap see only the work folder: no config, no home */
    char home[PATHSZ], tmpd[PATHSZ];
    snprintf(home, sizeof home, "%s/home", work_root);
    snprintf(tmpd, sizeof tmpd, "%s/tmp", work_root);
    mkdir(home, 0755);
    mkdir(tmpd, 0755);
    setenv("HOME", home, 1);
    setenv("TMPDIR", tmpd, 1);
    setenv("XDG_CONFIG_HOME", home, 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("GIT_AUTHOR_NAME", "lap-prop", 1);
    setenv("GIT_AUTHOR_EMAIL", "prop@lap", 1);
    setenv("GIT_COMMITTER_NAME", "lap-prop", 1);
    setenv("GIT_COMMITTER_EMAIL", "prop@lap", 1);
    setenv("LAP_USER", "lap-prop", 1);
    setenv("NO_COLOR", "1", 1);
    unsetenv("LAP_BRANCH");
    unsetenv("LAP_TEST_MERGE_FAIL_AFTER");

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    const char *jenv = getenv("LAP_PROP_JOBS");
    int jobs = jenv && *jenv ? atoi(jenv) : ncpu > 8 ? 8 : (int)ncpu;
    if (jobs < 1)
        jobs = 1;
    int running = 0;
    for (int seed = first; seed <= last; seed++) {
        if (running == jobs) {
            wait(NULL);
            running--;
        }
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0)
            run_child(seed);
        if (pid < 0)
            run_child_here(seed);
        else
            running++;
    }
    while (running > 0 && wait(NULL) > 0)
        running--;

    int failed = 0, cases = 0;
    merges = commits = reruns = stops_git_clean = 0;
    for (int seed = first; seed <= last; seed++) {
        char path[PATHSZ];
        snprintf(path, sizeof path, "%s/seed-%d.result", work_root, seed);
        char *r = read_file(path, NULL);
        int ok = 0, m = 0, c = 0, x = 0, g = 0, used = 0;
        cases++;
        if (!r || sscanf(r, "%d %d %d %d %d\n%n", &ok, &m, &c, &x, &g,
                         &used) < 5) {
            failed++;
            printf("FAIL seed %d: the case gave no verdict (a crash of the "
                   "driver)\n  kept: %s/seed-%d\n",
                   seed, work_root, seed);
            free(r);
            continue;
        }
        merges += m;
        commits += c;
        reruns += x;
        stops_git_clean += g;
        if (!ok) {
            failed++;
            printf("FAIL seed %d %s\n  kept: %s/seed-%d\n  replay: "
                   "LAP_PROP_SEED=%d make prop\n",
                   seed, r + used, work_root, seed, seed);
        }
        free(r);
        unlink(path);
    }
    printf("lap-prop: seeds %d-%d: %d cases, %d merges (%d stopped a file "
           "git merged cleanly), %d lap commits, %d cut reruns: %s\n",
           first, last, cases, merges, stops_git_clean, commits, reruns,
           failed ? "FAILED" : "ok");
    if (failed) {
        printf("%d of %d failed\n", failed, cases);
        return 1;
    }
    remove_tree(home);
    remove_tree(tmpd);
    rmdir(work_root);
    return 0;
}
