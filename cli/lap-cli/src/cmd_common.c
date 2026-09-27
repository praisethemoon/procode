#include "cmd.h"

#include "branches.h"

#include <stdarg.h>

static bool is_value_flag(const char *const *value_flags, const char *arg) {
    for (int32_t f = 0; value_flags && value_flags[f]; f++) {
        if (strcmp(arg, value_flags[f]) == 0)
            return true;
    }
    return false;
}

/* The value in "--flag=value" when arg is that form of `flag`, a long
 * flag; else NULL. */
static const char *eq_value(const char *arg, const char *flag) {
    size_t n = strlen(flag);
    if (strncmp(flag, "--", 2) != 0 || strncmp(arg, flag, n) != 0 ||
        arg[n] != '=')
        return NULL;
    return arg + n + 1;
}

/* True when arg is "--flag=value" for one of the value flags. */
static bool is_value_eq(const char *const *value_flags, const char *arg) {
    for (int32_t f = 0; value_flags && value_flags[f]; f++) {
        if (eq_value(arg, value_flags[f]))
            return true;
    }
    return false;
}

bool has_flag(int32_t argc, char **argv, const char *const *value_flags,
              const char *flag) {
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return false; /* flags end here */
        if (strcmp(argv[i], flag) == 0)
            return true;
        if (is_value_flag(value_flags, argv[i]))
            i++; /* never read a flag's value as a flag */
    }
    return false;
}

const char *flag_value(int32_t argc, char **argv,
                       const char *const *value_flags, const char *flag) {
    for (int32_t i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return NULL;
        if (strcmp(argv[i], flag) == 0)
            return i + 1 < argc ? argv[i + 1] : NULL;
        const char *eq = eq_value(argv[i], flag);
        if (eq)
            return eq;
        if (is_value_flag(value_flags, argv[i]))
            i++;
    }
    return NULL;
}

bool flags_known(int32_t argc, char **argv, const char *const *value_flags,
                 const char *const *bool_flags) {
    for (int32_t i = 0; i < argc; i++) {
        const char *w = argv[i];
        if (strcmp(w, "--") == 0)
            return true;
        if (is_value_flag(value_flags, w)) {
            if (i + 1 >= argc) {
                err_out(tty_json(), "usage", "%s needs a value", w);
                return false;
            }
            i++;
            continue;
        }
        if (w[0] != '-' || is_value_flag(bool_flags, w) ||
            is_value_eq(value_flags, w) ||
            strcmp(w, "--no-color") == 0 ||
            strncmp(w, "--color=", sizeof "--color=" - 1) == 0)
            continue;
        err_out(tty_json(), "unknown_flag", "unknown flag %s", w);
        return false;
    }
    return true;
}

const char *flag_value2(int32_t argc, char **argv,
                        const char *const *value_flags, const char *flag,
                        const char *alias) {
    const char *v = flag_value(argc, argv, value_flags, flag);
    return v ? v : flag_value(argc, argv, value_flags, alias);
}

const char *positional_arg(int32_t argc, char **argv,
                           const char *const *value_flags, int32_t index) {
    int32_t seen = 0;
    bool flags_ended = false;
    for (int32_t i = 0; i < argc; i++) {
        if (!flags_ended) {
            if (strcmp(argv[i], "--") == 0) {
                flags_ended = true;
                continue;
            }
            if (argv[i][0] == '-') {
                if (is_value_flag(value_flags, argv[i]))
                    i++; /* skip the flag's value */
                continue;
            }
        }
        if (seen == index)
            return argv[i];
        seen++;
    }
    return NULL;
}

void short_hash(const Rec *rec, char out[SHORT_HASH_LEN + 1]) {
    memcpy(out, rec->hash, SHORT_HASH_LEN);
    out[SHORT_HASH_LEN] = '\0';
}

void sb_indented(StrBuf *sb, const char *indent, const char *text) {
    while (*text) {
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        sb_puts(sb, indent);
        sb_text(sb, text, len);
        sb_putc(sb, '\n');
        if (!nl)
            break;
        text = nl + 1;
    }
}

void json_commit(StrBuf *sb, const Rec *rec, const char *note) {
    sb_printf(sb, "\"id\":\"%s\",\"hash\":\"%s\",\"ts\":\"%s\",\"user\":",
              rec->id, rec->hash, rec->ts);
    if (rec->user)
        json_escape_c(sb, rec->user);
    else
        sb_puts(sb, "null");
    sb_puts(sb, ",\"session\":");
    if (rec->session)
        sb_printf(sb, "\"%s\"", rec->session);
    else
        sb_puts(sb, "null");
    sb_puts(sb, ",\"file\":");
    json_escape_c(sb, rec->file);
    sb_printf(sb,
              ",\"op\":\"%s\",\"old_start\":%d,\"old_lines\":%d,"
              "\"new_start\":%d,\"new_lines\":%d,\"intent\":",
              rec->op, rec->old_start, rec->old_lines, rec->new_start,
              rec->new_lines);
    json_escape_c(sb, rec->intent);
    sb_puts(sb, ",\"behavior\":");
    json_escape_c(sb, rec->behavior);
    if (rec->forced)
        sb_puts(sb, ",\"forced\":true");
    if (rec->amended)
        sb_printf(sb, ",\"amended\":%d", rec->amended);
    if (rec->from)
        sb_printf(sb, ",\"from\":\"%s\"", rec->from);
    if (rec->lineage) {
        sb_puts(sb, ",\"branch\":");
        json_escape_c(sb, rec->lineage);
    }
    if (rec->session) { /* the session as it is named outside this folder */
        sb_puts(sb, ",\"session_ref\":");
        json_escape_c(sb, session_ref(sb->a, rec->lineage, rec->session));
    }
    if (note) {
        sb_puts(sb, ",\"match\":");
        json_escape_c(sb, note);
    }
}

void print_commit_human(StrBuf *sb, const Rec *rec, bool with_region,
                        const char *note) {
    const char *nl = strchr(rec->intent, '\n');
    int32_t mlen =
        nl ? (int32_t)(nl - rec->intent) : (int32_t)strlen(rec->intent);
    char sh[SHORT_HASH_LEN + 1];
    short_hash(rec, sh);
    sb_field(sb, S_ID, rec->id, 6);
    sb_putc(sb, ' ');
    sb_field(sb, S_MUTED, sh, 0);
    sb_putc(sb, ' ');
    char when[40];
    plat_ts_local(rec->ts, false, when);
    sb_field(sb, S_MUTED, when, 0);
    sb_puts(sb, "  ");
    sb_field(sb, S_SESSION,
             rec->session ? session_ref(sb->a, rec->lineage, rec->session)
                          : "-",
             6);
    sb_putc(sb, ' ');
    if (with_region) {
        Region shown = {rec->old_start, rec->old_lines, rec->new_start,
                        rec->new_lines};
        char desc[128];
        region_describe(&shown, desc, sizeof desc);
        sb_printf(sb, "%-8s ", rec->op);
        sb_text(sb, rec->file, strlen(rec->file));
        sb_puts(sb, "  ");
        sb_field(sb, S_MUTED, desc, 0);
        sb_putc(sb, '\n');
    } else {
        sb_text(sb, rec->file, strlen(rec->file));
        sb_putc(sb, '\n');
    }
    if (note) {
        sb_puts(sb, "       match: ");
        sb_text(sb, note, strlen(note));
        sb_putc(sb, '\n');
    }
    sb_puts(sb, "       ");
    sb_text(sb, rec->intent, (size_t)mlen);
    if (rec->amended) {
        sb_putc(sb, ' ');
        sb_field(sb, S_MUTED, amend_marker(sb->a, rec), 0);
    }
    sb_putc(sb, '\n');
}

const char *amend_marker(Arena *a, const Rec *rec) {
    if (rec->amended == 1)
        return "(amended)";
    char buf[48];
    snprintf(buf, sizeof buf, "(amended %d times)", rec->amended);
    return arena_strdup(a, buf);
}

bool ref_is_id(const char *ref) {
    if (ref[0] != 'L' || !ref[1])
        return false;
    for (const char *p = ref + 1; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
    }
    return true;
}

int32_t ref_find(const RecLog *log, const char *ref, const char **code,
                 char *err, size_t errsz) {
    *code = "unknown_ref";
    if (ref_is_id(ref)) {
        for (int32_t i = 0; i < log->count; i++) {
            if (log->v[i].type == REC_COMMIT &&
                strcmp(log->v[i].id, ref) == 0)
                return i;
        }
        snprintf(err, errsz, "no commit named %s", ref);
        return -1;
    }
    const char *h = ref[0] == '#' ? ref + 1 : ref;
    size_t n = strlen(h);
    char want[65];
    bool hex = n >= SHORT_HASH_LEN && n <= 64;
    for (size_t k = 0; hex && k < n; k++) {
        char c = h[k];
        if (c >= 'A' && c <= 'F')
            c = (char)(c - 'A' + 'a');
        hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        want[k] = c;
    }
    if (!hex) {
        snprintf(err, errsz,
                 "%s is not a commit: give an id (L42) or at least %d hex "
                 "digits of a hash",
                 ref, SHORT_HASH_LEN);
        return -1;
    }
    int32_t found = -1, matches = 0;
    char list[256] = "";
    for (int32_t i = 0; i < log->count; i++) {
        const Rec *rec = &log->v[i];
        if (rec->type != REC_COMMIT || memcmp(rec->hash, want, n) != 0)
            continue;
        if (matches++ == 0)
            found = i;
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "%s%s %.*s",
                 used ? ", " : "", rec->id, (int)SHORT_HASH_LEN, rec->hash);
    }
    if (matches == 1)
        return found;
    if (matches > 1) {
        *code = "ambiguous_ref";
        snprintf(err, errsz, "%s matches %d commits: %s", ref, matches, list);
        return -1;
    }
    snprintf(err, errsz, "no commit has a hash starting %.*s", (int)n, want);
    return -1;
}

void caches_sync_warn(Arena *a, const Repo *r) {
    char err[256];
    if (!idx_sync(a, r, err, sizeof err))
        fprintf(stderr, "lap: index sync: %s (run \"lap rebuild\")\n", err);
}

void err_out(bool json_mode, const char *code, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (json_mode) {
        /* minimal escaping: our own messages contain no quotes/control chars,
         * but paths could; escape conservatively */
        fputs("{\"ok\":false,\"error\":\"", stdout);
        fputs(code, stdout);
        fputs("\",\"message\":\"", stdout);
        for (const char *p = msg; *p; p++) {
            if (*p == '"' || *p == '\\')
                putchar('\\');
            if ((unsigned char)*p < 0x20)
                putchar(' ');
            else
                putchar(*p);
        }
        fputs("\"}\n", stdout);
    } else {
        fprintf(stderr, "%serror:%s %s\n", sgr_f(stderr, S_ERROR),
                sgr_off_f(stderr), msg);
    }
}

bool read_text_arg(Arena *a, const char *path, char **out, char *err,
                   size_t errsz) {
    char *text = NULL;
    size_t len = 0;
    if (strcmp(path, "-") == 0) {
        StrBuf sb;
        sb_init(&sb, a);
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, stdin)) > 0)
            sb_putn(&sb, buf, got);
        text = sb_finish(&sb);
    } else if (!plat_read_file(a, path, &text, &len)) {
        snprintf(err, errsz, "cannot read %s", path);
        return false;
    }
    msg_trim(text);
    *out = text;
    return true;
}

void region_describe(const Region *r, char *out, size_t outsz) {
    if (r->new_lines == 0 && r->old_lines > 0) {
        if (r->old_lines == 1)
            snprintf(out, outsz, "line %d (deleted)", r->old_start);
        else
            snprintf(out, outsz, "lines %d-%d (deleted)", r->old_start,
                     r->old_start + r->old_lines - 1);
    } else if (r->old_lines == 0 && r->new_lines > 0) {
        if (r->new_lines == 1)
            snprintf(out, outsz, "line %d (insertion)", r->new_start);
        else
            snprintf(out, outsz, "lines %d-%d (insertion)", r->new_start,
                     r->new_start + r->new_lines - 1);
    } else if (r->new_lines == 1) {
        snprintf(out, outsz, "line %d", r->new_start);
    } else {
        snprintf(out, outsz, "lines %d-%d", r->new_start,
                 r->new_start + r->new_lines - 1);
    }
}

/* The shadow store is a cache, so its absence must cost speed, not
 * answers: reconstruct the file's last-committed state from the log
 * instead of reporting an untracked file. */
static bool shadow_from_log(Arena *a, Repo *r, const char *rel, Lines *out) {
    uint64_t size = r->hist.size;
    if (!r->shadow_loaded || r->shadow_log_size != size) {
        /* In the command's arena, not a: a caller may hand each file a
         * scratch arena it resets, and this outlives the file. */
        r->shadow_idx = idx_ready(r->a, r);
        r->shadow_log = NULL;
        if (!r->shadow_idx) {
            RecLog *log = (RecLog *)arena_alloc(r->a, sizeof(RecLog));
            char err[256];
            if (repo_log_load(r->a, r, log, err, sizeof err))
                r->shadow_log = log;
        }
        r->shadow_log_size = size;
        r->shadow_loaded = true;
    }
    Lines st;
    bool deleted = false;
    Idx *ix = r->shadow_idx;
    if (ix) { /* the index knows every tracked file: no log read needed */
        if (idx_file_id(ix, rel) < 0 ||
            !snap_replay(a, r, ix, rel, -1, &st, &deleted) || deleted)
            return false;
        *out = st;
        return true;
    }
    RecLog *log = r->shadow_log;
    if (!log ||
        !rec_replay_file(a, log, rel, log->count - 1, &st, &deleted) ||
        deleted)
        return false;
    *out = st;
    return true;
}

bool file_diff_load(Arena *a, Repo *r, const char *rel, FileDiff *out,
                    char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    char wpath[LAP_PATH_MAX];
    snprintf(wpath, sizeof wpath, "%s/%s", r->root, rel);

    char *wdata = NULL, *sdata = NULL;
    size_t wlen = 0, slen = 0;

    out->work_exists = plat_is_file(wpath);
    if (out->work_exists) {
        if (!plat_read_file(a, wpath, &wdata, &wlen)) {
            snprintf(err, errsz, "cannot read %s (too large or unreadable)",
                     rel);
            return false;
        }
        if (looks_binary(wdata, wlen)) {
            out->binary = true;
            return true;
        }
    }
    if (!shadow_read(a, r, rel, &sdata, &slen, &out->shadow_exists)) {
        snprintf(err, errsz, "cannot read shadow copy of %s", rel);
        return false;
    }
    /* The working file is recorded as LF. The shadow stays byte for byte
     * what the log replays to, so only the comparison ignores its CRs. */
    out->work = lines_without_cr(a, split_lines(a, wdata ? wdata : "", wlen));
    out->shadow = split_lines(a, sdata ? sdata : "", slen);
    if (!out->shadow_exists)
        out->shadow_exists = shadow_from_log(a, r, rel, &out->shadow);
    if (out->work_exists && out->shadow_exists)
        out->regions =
            diff_lines(a, lines_without_cr(a, out->shadow), out->work);
    return true;
}

void sb_diff_line(StrBuf *sb, Style s, const char *indent, const char *sign,
                  Str text) {
    sb_puts(sb, indent);
    sb_puts(sb, sgr(s));
    sb_puts(sb, sign);
    sb_text(sb, text.ptr, text.len);
    sb_puts(sb, sgr_off());
    sb_putc(sb, 0x0a);
}

void render_commit_diff(StrBuf *sb, const Rec *rec) {
    sb_printf(sb, "%s@@ -%d,%d +%d,%d @@%s\n", sgr(S_HUNK), rec->old_start,
              rec->old_lines, rec->new_start, rec->new_lines, sgr_off());
    for (int32_t i = 0; i < rec->old_n; i++)
        sb_diff_line(sb, S_REMOVED, "", "- ", rec->old_text[i]);
    for (int32_t i = 0; i < rec->new_n; i++)
        sb_diff_line(sb, S_ADDED, "", "+ ", rec->new_text[i]);
}

const char *branch_given(int32_t argc, char **argv,
                         const char *const *value_flags) {
    const char *flag = flag_value(argc, argv, value_flags, "--branch");
    if (flag)
        return flag;
    const char *env = getenv("LAP_BRANCH");
    return env && env[0] ? env : NULL;
}

bool branch_check(Arena *a, const Repo *r, const char *given, bool json) {
    bool is_branch = r->hist.parent[0] != '\0';
    const char *mine = is_branch ? r->hist.name : LAP_MAIN_LINEAGE;
    if (given) {
        if (strcmp(given, mine) == 0 ||
            (is_branch && strcmp(given, r->hist.lineage) == 0))
            return true;
        err_out(json, "wrong_branch",
                "this folder (%s) records to %s, not %s: you may be in the "
                "wrong folder; here, pass --branch %s",
                r->root, mine, given, mine);
        return false;
    }
    Branches reg;
    branches_load(a, r->lapdir, &reg);
    if (!is_branch && reg.n == 0)
        return true;
    err_out(json, "branch_required",
            "this folder (%s) %s: say which line of history this records "
            "to with --branch %s (or LAP_BRANCH=%s)",
            r->root, is_branch ? "is a branch" : "has branches", mine, mine);
    return false;
}

const char *branch_find(Arena *a, const Repo *r, const Branches *reg,
                        const char *key) {
    const BranchEntry *e = branches_find(reg, key);
    if (e)
        return e->id;
    const char **ids;
    int32_t n = hist_lineages(a, r->lapdir, &ids);
    for (int32_t i = 0; i < n; i++) {
        Rec br;
        char err[256];
        if (hist_first_record(a, r->lapdir, ids[i], &br, err, sizeof err) &&
            br.type == REC_BRANCH &&
            (strcmp(br.id, key) == 0 || strcmp(br.name, key) == 0))
            return ids[i];
    }
    return NULL;
}

bool branch_folder_is(Arena *a, const char *path, const char *id) {
    char lapdir[LAP_PATH_MAX], lineage[HIST_LINEAGE_MAX], err[256];
    snprintf(lapdir, sizeof lapdir, "%s/%s", path, LAP_DIR);
    return plat_is_dir(lapdir) &&
           hist_folder_lineage(a, lapdir, lineage, err, sizeof err) &&
           strcmp(lineage, id) == 0;
}

bool repo_view_branch(Arena *a, Repo *r, const char *name, bool json) {
    if (!name)
        return true;
    bool is_branch = r->hist.parent[0] != '\0';
    if ((is_branch && (strcmp(name, r->hist.name) == 0 ||
                       strcmp(name, r->hist.lineage) == 0)) ||
        (!is_branch && strcmp(name, LAP_MAIN_LINEAGE) == 0))
        return true; /* this folder's own history */
    Hist view;
    char err[512];
    bool ok;
    if (strcmp(name, LAP_MAIN_LINEAGE) == 0) {
        ok = hist_open(a, r->lapdir, LAP_MAIN_LINEAGE, &view, err, sizeof err);
    } else {
        Branches reg;
        branches_load_deep(a, r->lapdir, &reg);
        const char *id = branch_find(a, r, &reg, name);
        const BranchEntry *e = id ? branches_find(&reg, id) : NULL;
        if (!id) {
            err_out(json, "unknown_branch",
                    "no branch %s here: not in this folder's registry, and "
                    "no chunk of it in %s",
                    name, r->hist.dir);
            return false;
        }
        if (e && branch_folder_is(a, e->path, id)) {
            char lapdir[LAP_PATH_MAX];
            snprintf(lapdir, sizeof lapdir, "%s/%s", e->path, LAP_DIR);
            ok = hist_open_folder(a, lapdir, &view, err, sizeof err);
        } else {
            ok = hist_open_lineage(a, r->lapdir, id, &view, err, sizeof err);
        }
    }
    if (!ok) {
        err_out(json, "unknown_branch", "cannot read branch %s: %s", name,
                err);
        return false;
    }
    r->hist = view;
    r->foreign = true;
    return true;
}

const char *session_ref(Arena *a, const char *lineage, const char *sid) {
    if (!sid || !lineage || strcmp(lineage, LAP_MAIN_LINEAGE) == 0)
        return sid;
    return arena_printf(a, "%s/%s", lineage, sid);
}

int32_t own_part_start(const RecLog *log, const char *lineage) {
    int32_t own = 0;
    for (int32_t i = 0; lineage && i < log->count; i++) {
        if (log->v[i].type == REC_BRANCH && strcmp(log->v[i].id, lineage) == 0)
            own = i + 1;
    }
    return own;
}

bool session_resolve(Arena *a, Repo *r, const char *ref, const char **sid,
                     bool *adopted, bool json) {
    *adopted = false;
    const char *slash = ref ? strrchr(ref, '/') : NULL;
    if (!slash) {
        *sid = ref;
        return true;
    }
    const char *name = arena_strndup(a, ref, (size_t)(slash - ref));
    const char *id = slash + 1;
    Repo view = *r;
    if (!repo_view_branch(a, &view, name, json))
        return false;
    *sid = id;
    /* the branch's own session_start — after its branch record: a session
     * from before its base is the parent's, not the branch's — then whether
     * lap merge adopted it here */
    RecLog blog, mine;
    char err[512];
    const char *hash = NULL;
    if (repo_log_load(a, &view, &blog, err, sizeof err)) {
        int32_t own = own_part_start(
            &blog, view.hist.parent[0] ? view.hist.lineage : NULL);
        for (int32_t i = own; i < blog.count; i++) {
            if (blog.v[i].type == REC_SESSION_START &&
                strcmp(blog.v[i].id, id) == 0)
                hash = blog.v[i].hash;
        }
    }
    if (!hash) {
        err_out(json, "unknown_session", "branch %s has no session %s", name,
                id);
        return false;
    }
    if (!view.foreign) /* the branch is this folder's own */
        return true;
    if (repo_log_load(a, r, &mine, err, sizeof err)) {
        for (int32_t i = 0; i < mine.count; i++) {
            const Rec *s = &mine.v[i];
            if (s->type == REC_SESSION_START && s->from &&
                strcmp(s->from, hash) == 0) {
                *sid = s->id;
                *adopted = true;
                return true;
            }
        }
    }
    *r = view;
    return true;
}
