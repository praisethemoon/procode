#include "cmd.h"

#include <stdarg.h>

static bool is_value_flag(const char *const *value_flags, const char *arg) {
    for (int32_t f = 0; value_flags && value_flags[f]; f++) {
        if (strcmp(arg, value_flags[f]) == 0)
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
        if (is_value_flag(value_flags, argv[i]))
            i++;
    }
    return NULL;
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

void json_commit(StrBuf *sb, const Rec *rec, const char *note) {
    sb_printf(sb, "\"id\":\"%s\",\"ts\":\"%s\",\"user\":", rec->id,
              rec->ts);
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
              "\"new_start\":%d,\"new_lines\":%d,\"msg\":",
              rec->op, rec->old_start, rec->old_lines, rec->new_start,
              rec->new_lines);
    json_escape_c(sb, rec->msg);
    if (note) {
        sb_puts(sb, ",\"match\":");
        json_escape_c(sb, note);
    }
}

void print_commit_human(StrBuf *sb, const Rec *rec, bool with_region,
                        const char *note) {
    const char *nl = strchr(rec->msg, '\n');
    int32_t mlen = nl ? (int32_t)(nl - rec->msg) : (int32_t)strlen(rec->msg);
    sb_field(sb, S_ID, rec->id, 6);
    sb_putc(sb, ' ');
    sb_field(sb, S_MUTED, rec->ts, 0);
    sb_puts(sb, "  ");
    sb_field(sb, S_SESSION, rec->session ? rec->session : "-", 6);
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
    sb_text(sb, rec->msg, (size_t)mlen);
    sb_putc(sb, '\n');
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

int32_t message_arg(Arena *a, int32_t argc, char **argv,
                    const char *const *value_flags, const char **out,
                    char *err, size_t errsz) {
    const char *m = flag_value(argc, argv, value_flags, "-m");
    const char *f = flag_value(argc, argv, value_flags, "-F");
    *out = NULL;
    if (m && f) {
        snprintf(err, errsz, "-m and -F are mutually exclusive");
        return -1;
    }
    char *text = NULL;
    size_t len = 0;
    if (m) {
        text = arena_strdup(a, m);
        len = strlen(text);
    } else if (f) {
        if (strcmp(f, "-") == 0) {
            StrBuf sb;
            sb_init(&sb, a);
            char buf[4096];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, stdin)) > 0)
                sb_putn(&sb, buf, got);
            len = sb.len;
            text = sb_finish(&sb);
        } else if (!plat_read_file(a, f, &text, &len)) {
            snprintf(err, errsz, "cannot read message file %s", f);
            return -1;
        }
    } else {
        return 0;
    }
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t'))
        len--;
    text[len] = '\0';
    if (len == 0) {
        snprintf(err, errsz, "message is empty");
        return -1;
    }
    *out = text;
    return 1;
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
    Lines st;
    bool deleted = false;
    Idx *ix = idx_ready(a, r);
    if (ix) { /* the index knows every tracked file: no log read needed */
        if (idx_file_id(ix, rel) < 0 ||
            !snap_replay(a, r, ix, rel, -1, &st, &deleted) || deleted)
            return false;
        *out = st;
        return true;
    }
    RecLog log;
    char err[256];
    if (!rec_log_load(a, r->logpath, &log, err, sizeof err) ||
        !rec_replay_file(a, &log, rel, log.count - 1, &st, &deleted) ||
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
    if (!shadow_read(r, rel, &sdata, &slen, &out->shadow_exists)) {
        snprintf(err, errsz, "cannot read shadow copy of %s", rel);
        return false;
    }
    out->work = split_lines(a, wdata ? wdata : "", wlen);
    out->shadow = split_lines(a, sdata ? sdata : "", slen);
    if (!out->shadow_exists)
        out->shadow_exists = shadow_from_log(a, r, rel, &out->shadow);
    if (out->work_exists && out->shadow_exists)
        out->regions = diff_lines(a, out->shadow, out->work);
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
