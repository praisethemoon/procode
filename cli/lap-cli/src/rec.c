#include "rec.h"

#include "json.h"
#include "platform.h"
#include "sha256.h"

static const char *type_name(RecType t) {
    switch (t) {
    case REC_INIT: return "init";
    case REC_COMMIT: return "commit";
    case REC_SESSION_START: return "session_start";
    case REC_SESSION_END: return "session_end";
    }
    return "?";
}

static void put_text_array(StrBuf *sb, const char *key, const Str *lines,
                           int32_t n) {
    sb_printf(sb, ",\"%s\":[", key);
    for (int32_t i = 0; i < n; i++) {
        if (i)
            sb_putc(sb, ',');
        json_escape(sb, lines[i].ptr, lines[i].len);
    }
    sb_putc(sb, ']');
}

char *rec_encode(Arena *a, Rec *rec, size_t *out_len) {
    StrBuf sb;
    sb_init(&sb, a);
    sb_printf(&sb, "{\"type\":\"%s\"", type_name(rec->type));
    switch (rec->type) {
    case REC_INIT:
        sb_printf(&sb, ",\"version\":%d", rec->version);
        break;
    case REC_COMMIT:
        sb_printf(&sb, ",\"id\":\"%s\"", rec->id);
        if (rec->user) {
            sb_puts(&sb, ",\"user\":");
            json_escape_c(&sb, rec->user);
        }
        if (rec->session)
            sb_printf(&sb, ",\"session\":\"%s\"", rec->session);
        else
            sb_puts(&sb, ",\"session\":null");
        sb_puts(&sb, ",\"file\":");
        json_escape_c(&sb, rec->file);
        sb_printf(&sb, ",\"op\":\"%s\"", rec->op);
        sb_printf(&sb,
                  ",\"old_start\":%d,\"old_lines\":%d,\"new_start\":%d,"
                  "\"new_lines\":%d",
                  rec->old_start, rec->old_lines, rec->new_start,
                  rec->new_lines);
        sb_printf(&sb, ",\"eof_nl\":%s", rec->eof_nl ? "true" : "false");
        put_text_array(&sb, "old_text", rec->old_text, rec->old_n);
        put_text_array(&sb, "new_text", rec->new_text, rec->new_n);
        sb_puts(&sb, ",\"msg\":");
        json_escape_c(&sb, rec->msg);
        break;
    case REC_SESSION_START:
        sb_printf(&sb, ",\"id\":\"%s\"", rec->id);
        if (rec->user) {
            sb_puts(&sb, ",\"user\":");
            json_escape_c(&sb, rec->user);
        }
        sb_puts(&sb, ",\"msg\":");
        json_escape_c(&sb, rec->msg);
        if (rec->meta_n > 0)
            rec_meta_json(&sb, rec);
        break;
    case REC_SESSION_END:
        sb_printf(&sb, ",\"id\":\"%s\"", rec->id);
        break;
    }
    sb_printf(&sb, ",\"ts\":\"%s\"", rec->ts);
    sb_printf(&sb, ",\"prev\":\"%s\"}", rec->prev);
    size_t len = sb.len;
    char *line = sb_finish(&sb);
    sha256_hex(line, len, rec->hash);
    rec->raw = line;
    rec->raw_len = len;
    if (out_len)
        *out_len = len;
    return line;
}

static bool get_text_array(Arena *a, const JVal *obj, const char *key,
                           Str **out, int32_t *out_n) {
    JVal *v = jobj_get(obj, key);
    if (!v || v->t != J_ARR)
        return false;
    Str *arr = (Str *)arena_alloc(a, (v->arr.n ? v->arr.n : 1) * sizeof(Str));
    for (size_t i = 0; i < v->arr.n; i++) {
        if (v->arr.items[i]->t != J_STR)
            return false;
        arr[i] = v->arr.items[i]->s;
    }
    *out = arr;
    *out_n = (int32_t)v->arr.n;
    return true;
}

bool rec_decode(Arena *a, const char *line, size_t len, Rec *out, char *err,
                size_t errsz) {
    memset(out, 0, sizeof(*out));
    JVal *v = json_parse(a, line, len, err, errsz);
    if (!v)
        return false;
    if (v->t != J_OBJ) {
        snprintf(err, errsz, "record is not a JSON object");
        return false;
    }
    const char *type = jobj_str(v, "type");
    if (!type) {
        snprintf(err, errsz, "record missing \"type\"");
        return false;
    }
    if (strcmp(type, "init") == 0) {
        out->type = REC_INIT;
        out->version = (int32_t)jobj_int(v, "version", 1);
    } else if (strcmp(type, "commit") == 0) {
        out->type = REC_COMMIT;
        out->id = jobj_str(v, "id");
        out->user = jobj_str(v, "user"); /* NULL in pre-user logs */
        out->session = jobj_str(v, "session"); /* NULL for json null */
        out->file = jobj_str(v, "file");
        out->op = jobj_str(v, "op");
        out->msg = jobj_str(v, "msg");
        out->old_start = (int32_t)jobj_int(v, "old_start", 0);
        out->old_lines = (int32_t)jobj_int(v, "old_lines", 0);
        out->new_start = (int32_t)jobj_int(v, "new_start", 0);
        out->new_lines = (int32_t)jobj_int(v, "new_lines", 0);
        out->eof_nl = jobj_bool(v, "eof_nl", true);
        if (!out->id || !out->file || !out->op || !out->msg) {
            snprintf(err, errsz, "commit record missing required field");
            return false;
        }
        if (!get_text_array(a, v, "old_text", &out->old_text, &out->old_n) ||
            !get_text_array(a, v, "new_text", &out->new_text, &out->new_n)) {
            snprintf(err, errsz, "commit record has bad text arrays");
            return false;
        }
        if (out->old_n != out->old_lines || out->new_n != out->new_lines) {
            snprintf(err, errsz, "commit record line counts disagree");
            return false;
        }
    } else if (strcmp(type, "session_start") == 0) {
        out->type = REC_SESSION_START;
        out->id = jobj_str(v, "id");
        out->user = jobj_str(v, "user");
        out->msg = jobj_str(v, "msg");
        if (!out->id || !out->msg) {
            snprintf(err, errsz, "session_start record missing field");
            return false;
        }
        JVal *meta = jobj_get(v, "meta");
        if (meta) {
            if (meta->t != J_OBJ) {
                snprintf(err, errsz, "session_start meta is not an object");
                return false;
            }
            size_t n = meta->obj.n ? meta->obj.n : 1;
            out->meta_keys = (const char **)arena_alloc(a, n * sizeof(char *));
            out->meta_vals = (const char **)arena_alloc(a, n * sizeof(char *));
            for (size_t i = 0; i < meta->obj.n; i++) {
                if (meta->obj.vals[i]->t != J_STR) {
                    snprintf(err, errsz, "session_start meta value is not a "
                                         "string");
                    return false;
                }
                out->meta_keys[i] = meta->obj.keys[i].ptr;
                out->meta_vals[i] = meta->obj.vals[i]->s.ptr;
            }
            out->meta_n = (int32_t)meta->obj.n;
        }
    } else if (strcmp(type, "session_end") == 0) {
        out->type = REC_SESSION_END;
        out->id = jobj_str(v, "id");
        if (!out->id) {
            snprintf(err, errsz, "session_end record missing id");
            return false;
        }
    } else {
        snprintf(err, errsz, "unknown record type \"%s\"", type);
        return false;
    }
    out->ts = jobj_str(v, "ts");
    out->prev = jobj_str(v, "prev");
    if (!out->ts || !out->prev) {
        snprintf(err, errsz, "record missing ts/prev");
        return false;
    }
    out->raw = line;
    out->raw_len = len;
    sha256_hex(line, len, out->hash);
    return true;
}

bool rec_log_load(Arena *a, const char *path, RecLog *out, char *err,
                  size_t errsz) {
    memset(out, 0, sizeof(*out));
    out->chain_ok = true;
    out->chain_break_index = -1;
    char *data;
    size_t len;
    /* lap's own log must never become unreadable by growing: no size cap */
    if (!plat_read_file_max(a, path, &data, &len, (size_t)-1)) {
        snprintf(err, errsz, "cannot read log file %s", path);
        return false;
    }
    /* Drop a torn (unterminated) final line: a crash mid-append wrote it,
     * no one ever acknowledged it. */
    if (len > 0 && data[len - 1] != '\n') {
        size_t cut = len;
        while (cut > 0 && data[cut - 1] != '\n')
            cut--;
        out->torn_tail = true;
        out->torn_bytes = (uint64_t)(len - cut);
        len = cut;
    }
    Lines l = split_lines(a, data, len);
    Rec *recs = (Rec *)arena_alloc(a, (size_t)(l.count ? l.count : 1) *
                                          sizeof(Rec));
    int32_t n = 0;
    const char *prev_hash = LAP_HASH_ZERO;
    for (int32_t i = 0; i < l.count; i++) {
        if (l.lines[i].len == 0)
            continue;
        char lerr[256];
        if (!rec_decode(a, l.lines[i].ptr, l.lines[i].len, &recs[n], lerr,
                        sizeof lerr)) {
            snprintf(err, errsz, "log line %d: %s", i + 1, lerr);
            return false;
        }
        if (out->chain_ok && strcmp(recs[n].prev, prev_hash) != 0) {
            out->chain_ok = false;
            out->chain_break_index = n;
            snprintf(out->chain_err, sizeof out->chain_err,
                     "hash chain broken at log line %d (record %s)", i + 1,
                     recs[n].id ? recs[n].id : "init");
        }
        prev_hash = recs[n].hash;
        n++;
    }
    out->v = recs;
    out->count = n;
    return true;
}

uint32_t rec_session_no(const char *id) {
    if (!id || id[0] != 'S' || !id[1])
        return 0;
    char *end;
    unsigned long v = strtoul(id + 1, &end, 10);
    return *end == '\0' && v > 0 && v <= UINT32_MAX ? (uint32_t)v : 0;
}

void rec_apply(Arena *a, Lines *cur, const Rec *rec) {
    if (strcmp(rec->op, "delete") == 0) {
        cur->lines = NULL;
        cur->count = 0;
        cur->eof_nl = true;
        return;
    }
    Lines next;
    next.eof_nl = rec->eof_nl;
    /* worst-case size; coordinates from a damaged record are clamped below
     * rather than trusted */
    next.lines = (Str *)arena_alloc(
        a, (size_t)(cur->count + rec->new_n + 1) * sizeof(Str));
    Str dummy = {NULL, 0};
    const Str *curl = cur->lines ? cur->lines : &dummy;
    int32_t head = rec->old_start - 1;
    if (head < 0)
        head = 0;
    int32_t tail = head + rec->old_lines;
    if (tail < 0)
        tail = 0;
    int32_t w = 0;
    for (int32_t k = 0; k < head && k < cur->count; k++)
        next.lines[w++] = curl[k];
    for (int32_t k = 0; k < rec->new_n; k++)
        next.lines[w++] = rec->new_text[k];
    for (int32_t k = tail; k < cur->count; k++)
        next.lines[w++] = curl[k];
    next.count = w;
    *cur = next;
}

bool rec_replay_file(Arena *a, const RecLog *log, const char *rel,
                     int32_t upto_index, Lines *out, bool *deleted) {
    Lines cur;
    cur.lines = NULL;
    cur.count = 0;
    cur.eof_nl = true;
    bool seen = false;
    bool is_deleted = false;
    for (int32_t i = 0; i <= upto_index && i < log->count; i++) {
        const Rec *rec = &log->v[i];
        if (rec->type != REC_COMMIT || strcmp(rec->file, rel) != 0)
            continue;
        seen = true;
        if (strcmp(rec->op, "delete") == 0) {
            cur.lines = NULL;
            cur.count = 0;
            cur.eof_nl = true;
            is_deleted = true;
            continue;
        }
        is_deleted = false;
        rec_apply(a, &cur, rec);
    }
    if (!seen)
        return false;
    *out = cur;
    *deleted = is_deleted;
    return true;
}

const char *rec_meta(const Rec *rec, const char *key) {
    for (int32_t i = 0; i < rec->meta_n; i++) {
        if (strcmp(rec->meta_keys[i], key) == 0)
            return rec->meta_vals[i];
    }
    return NULL;
}

void rec_meta_json(StrBuf *sb, const Rec *rec) {
    sb_puts(sb, ",\"meta\":{");
    for (int32_t i = 0; i < rec->meta_n; i++) {
        if (i)
            sb_putc(sb, ',');
        json_escape_c(sb, rec->meta_keys[i]);
        sb_putc(sb, ':');
        json_escape_c(sb, rec->meta_vals[i]);
    }
    sb_putc(sb, '}');
}
