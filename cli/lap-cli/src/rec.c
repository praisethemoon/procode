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
    case REC_BRANCH: return "branch";
    case REC_MERGE: return "merge";
    case REC_AMEND: return "amend";
    case REC_UNKNOWN: break; /* never written by this lap */
    }
    return "?";
}

const char *rec_type_name(RecType t) {
    return type_name(t);
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

void rec_note_newer(const char *type) {
    static bool told; /* once a command */
    if (told)
        return;
    told = true;
    if (type)
        fprintf(stderr, "note: this history has records of a newer type "
                        "(\"%s\") that this lap does not know: they are "
                        "skipped here. Update lap to see them.\n",
                type);
    else
        fprintf(stderr, "note: this history has records of a newer type "
                        "that this lap does not know: they are skipped "
                        "here. Update lap to see them.\n");
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
        sb_puts(&sb, ",\"intent\":");
        json_escape_c(&sb, rec->intent);
        sb_puts(&sb, ",\"behavior\":");
        json_escape_c(&sb, rec->behavior);
        if (rec->forced)
            sb_puts(&sb, ",\"forced\":true");
        if (rec->from)
            sb_printf(&sb, ",\"from\":\"%s\"", rec->from);
        break;
    case REC_SESSION_START:
        sb_printf(&sb, ",\"id\":\"%s\"", rec->id);
        if (rec->user) {
            sb_puts(&sb, ",\"user\":");
            json_escape_c(&sb, rec->user);
        }
        sb_puts(&sb, ",\"msg\":");
        json_escape_c(&sb, rec->msg);
        rec_meta_json(&sb, rec);
        if (rec->from)
            sb_printf(&sb, ",\"from\":\"%s\"", rec->from);
        break;
    case REC_SESSION_END:
        sb_printf(&sb, ",\"id\":\"%s\"", rec->id);
        /* each part only when given: a line with none reads as before */
        if (rec->sum_done) {
            sb_puts(&sb, ",\"done\":");
            json_escape_c(&sb, rec->sum_done);
        }
        if (rec->sum_decided) {
            sb_puts(&sb, ",\"decided\":");
            json_escape_c(&sb, rec->sum_decided);
        }
        if (rec->sum_left) {
            sb_puts(&sb, ",\"left\":");
            json_escape_c(&sb, rec->sum_left);
        }
        if (rec->from)
            sb_printf(&sb, ",\"from\":\"%s\"", rec->from);
        break;
    case REC_BRANCH:
        sb_printf(&sb, ",\"id\":\"%s\"", rec->id);
        sb_puts(&sb, ",\"name\":");
        json_escape_c(&sb, rec->name);
        sb_printf(&sb, ",\"parent\":\"%s\",\"base\":\"%s\",\"base_chunk\":%d",
                  rec->parent, rec->base, rec->base_chunk);
        if (rec->user) {
            sb_puts(&sb, ",\"user\":");
            json_escape_c(&sb, rec->user);
        }
        break;
    case REC_MERGE:
        sb_printf(&sb, ",\"branch\":\"%s\"", rec->branch);
        sb_puts(&sb, ",\"name\":");
        json_escape_c(&sb, rec->name);
        sb_printf(&sb, ",\"head\":\"%s\",\"adopted\":%d,\"left\":%d",
                  rec->head, rec->adopted, rec->left);
        sb_puts(&sb, ",\"stopped\":[");
        for (int32_t i = 0; i < rec->stopped_n; i++) {
            sb_puts(&sb, i ? ",{\"file\":" : "{\"file\":");
            json_escape_c(&sb, rec->stopped_file[i]);
            sb_printf(&sb, ",\"at\":\"%s\"}", rec->stopped_at[i]);
        }
        sb_putc(&sb, ']');
        if (rec->already_n > 0) { /* absent when none: older records */
            sb_puts(&sb, ",\"already\":[");
            for (int32_t i = 0; i < rec->already_n; i++)
                sb_printf(&sb, i ? ",\"%s\"" : "\"%s\"", rec->already[i]);
            sb_putc(&sb, ']');
        }
        if (rec->user) {
            sb_puts(&sb, ",\"user\":");
            json_escape_c(&sb, rec->user);
        }
        break;
    case REC_AMEND:
        sb_printf(&sb, ",\"of\":\"%s\"", rec->of);
        if (rec->from)
            sb_printf(&sb, ",\"from\":\"%s\"", rec->from);
        if (rec->user) {
            sb_puts(&sb, ",\"user\":");
            json_escape_c(&sb, rec->user);
        }
        sb_puts(&sb, ",\"intent\":");
        json_escape_c(&sb, rec->intent);
        sb_puts(&sb, ",\"behavior\":");
        json_escape_c(&sb, rec->behavior);
        if (rec->forced)
            sb_puts(&sb, ",\"forced\":true");
        break;
    case REC_UNKNOWN: /* only ever read: a writer refuses such a history */
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
        out->intent = jobj_str(v, "intent");
        out->behavior = jobj_str(v, "behavior");
        out->forced = jobj_bool(v, "forced", false);
        out->from = jobj_str(v, "from");
        out->old_start = (int32_t)jobj_int(v, "old_start", 0);
        out->old_lines = (int32_t)jobj_int(v, "old_lines", 0);
        out->new_start = (int32_t)jobj_int(v, "new_start", 0);
        out->new_lines = (int32_t)jobj_int(v, "new_lines", 0);
        out->eof_nl = jobj_bool(v, "eof_nl", true);
        if (!out->id || !out->file || !out->op) {
            snprintf(err, errsz, "commit record missing required field");
            return false;
        }
        if (!out->intent || !out->behavior || !out->intent[0] ||
            !out->behavior[0]) {
            snprintf(err, errsz, "commit %s has no intent and behavior",
                     out->id);
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
        out->from = jobj_str(v, "from");
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
                const JVal *mv = meta->obj.vals[i];
                StrBuf vb;
                sb_init(&vb, a);
                if (mv->t == J_STR)
                    json_escape(&vb, mv->s.ptr, mv->s.len);
                else if (mv->t == J_BOOL)
                    sb_puts(&vb, mv->b ? "true" : "false");
                else if (mv->t == J_NUM && mv->is_int)
                    sb_printf(&vb, "%lld", (long long)mv->i);
                else if (mv->t == J_NUM)
                    sb_printf(&vb, "%.17g", mv->num);
                else {
                    snprintf(err, errsz, "session_start meta value must be a "
                                         "string, number or boolean");
                    return false;
                }
                out->meta_keys[i] = meta->obj.keys[i].ptr;
                out->meta_vals[i] = sb_finish(&vb);
            }
            out->meta_n = (int32_t)meta->obj.n;
        }
    } else if (strcmp(type, "session_end") == 0) {
        out->type = REC_SESSION_END;
        out->id = jobj_str(v, "id");
        out->sum_done = jobj_str(v, "done");
        out->sum_decided = jobj_str(v, "decided");
        out->sum_left = jobj_str(v, "left");
        out->from = jobj_str(v, "from");
        if (!out->id) {
            snprintf(err, errsz, "session_end record missing id");
            return false;
        }
    } else if (strcmp(type, "branch") == 0) {
        out->type = REC_BRANCH;
        out->id = jobj_str(v, "id");
        out->name = jobj_str(v, "name");
        out->parent = jobj_str(v, "parent");
        out->base = jobj_str(v, "base");
        out->base_chunk = (int32_t)jobj_int(v, "base_chunk", 0);
        out->user = jobj_str(v, "user");
        if (!out->id || !out->name || !out->parent || !out->base ||
            out->base_chunk < 1) {
            snprintf(err, errsz, "branch record missing field");
            return false;
        }
    } else if (strcmp(type, "amend") == 0) {
        out->type = REC_AMEND;
        out->of = jobj_str(v, "of");
        out->from = jobj_str(v, "from");
        out->user = jobj_str(v, "user");
        out->intent = jobj_str(v, "intent");
        out->behavior = jobj_str(v, "behavior");
        out->forced = jobj_bool(v, "forced", false);
        if (!out->of || !out->intent || !out->behavior) {
            snprintf(err, errsz, "amend record missing field");
            return false;
        }
    } else if (strcmp(type, "merge") == 0) {
        out->type = REC_MERGE;
        out->branch = jobj_str(v, "branch");
        out->name = jobj_str(v, "name");
        out->head = jobj_str(v, "head");
        out->adopted = (int32_t)jobj_int(v, "adopted", 0);
        out->left = (int32_t)jobj_int(v, "left", 0);
        out->user = jobj_str(v, "user");
        JVal *st = jobj_get(v, "stopped");
        if (!out->branch || !out->name || !out->head || !st ||
            st->t != J_ARR) {
            snprintf(err, errsz, "merge record missing field");
            return false;
        }
        size_t n = st->arr.n ? st->arr.n : 1;
        out->stopped_file = (const char **)arena_alloc(a, n * sizeof(char *));
        out->stopped_at = (const char **)arena_alloc(a, n * sizeof(char *));
        for (size_t i = 0; i < st->arr.n; i++) {
            const char *f = jobj_str(st->arr.items[i], "file");
            const char *at = jobj_str(st->arr.items[i], "at");
            if (!f || !at) {
                snprintf(err, errsz, "merge record has a bad stopped entry");
                return false;
            }
            out->stopped_file[i] = f;
            out->stopped_at[i] = at;
        }
        out->stopped_n = (int32_t)st->arr.n;
        JVal *al = jobj_get(v, "already");
        if (al) {
            if (al->t != J_ARR) {
                snprintf(err, errsz, "merge record has a bad already list");
                return false;
            }
            size_t an = al->arr.n ? al->arr.n : 1;
            out->already = (const char **)arena_alloc(a, an * sizeof(char *));
            for (size_t i = 0; i < al->arr.n; i++) {
                if (al->arr.items[i]->t != J_STR) {
                    snprintf(err, errsz,
                             "merge record has a bad already entry");
                    return false;
                }
                out->already[i] = al->arr.items[i]->s.ptr;
            }
            out->already_n = (int32_t)al->arr.n;
        }
    } else {
        /* written by a newer lap: kept in the chain, not interpreted */
        out->type = REC_UNKNOWN;
        out->name = type;
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

static void where_line(const char *data, uint64_t off, char *out,
                       size_t outsz) {
    int32_t line = 1;
    for (uint64_t p = 0; p < off; p++) {
        if (data[p] == '\n')
            line++;
    }
    snprintf(out, outsz, "log line %d", line);
}

void rec_amend_one(Arena *a, Rec *c, const Rec *am, int32_t room) {
    if (!c->earlier_intent) {
        size_t n = (size_t)(room > 0 ? room : 1);
        c->earlier_intent = (const char **)arena_alloc(a, n * sizeof(char *));
        c->earlier_behavior =
            (const char **)arena_alloc(a, n * sizeof(char *));
        c->earlier_user = (const char **)arena_alloc(a, n * sizeof(char *));
        c->earlier_ts = (const char **)arena_alloc(a, n * sizeof(char *));
    }
    /* the text being replaced: the commit's own, then each amendment's,
     * written by whoever wrote it, when they did */
    int32_t k = c->earlier_n++;
    c->earlier_intent[k] = c->intent;
    c->earlier_behavior[k] = c->behavior;
    c->earlier_user[k] = c->amended ? c->amend_user : c->user;
    c->earlier_ts[k] = c->amended ? c->amend_ts : c->ts;
    c->intent = am->intent;
    c->behavior = am->behavior;
    c->forced = am->forced;
    c->amend_user = am->user;
    c->amend_ts = am->ts;
    c->amended++;
}

void rec_amend_log(Arena *a, RecLog *log) {
    int32_t amends = 0;
    for (int32_t i = 0; i < log->count; i++)
        amends += log->v[i].type == REC_AMEND;
    if (amends == 0)
        return;
    /* each amendment's commit, found by hash; then how many each has, so
     * its earlier texts get room for all of them */
    int32_t *target = (int32_t *)arena_alloc(
        a, (size_t)log->count * sizeof(int32_t));
    int32_t *room = (int32_t *)arena_alloc0(
        a, (size_t)log->count * sizeof(int32_t));
    for (int32_t i = 0; i < log->count; i++) {
        target[i] = -1;
        if (log->v[i].type != REC_AMEND)
            continue;
        for (int32_t j = i - 1; j >= 0; j--) {
            if (log->v[j].type == REC_COMMIT &&
                strcmp(log->v[j].hash, log->v[i].of) == 0) {
                target[i] = j;
                room[j]++;
                break;
            }
        }
    }
    for (int32_t i = 0; i < log->count; i++) {
        if (target[i] >= 0)
            rec_amend_one(a, &log->v[target[i]], &log->v[i],
                          room[target[i]]);
    }
}

bool rec_log_parse(Arena *a, const char *data, size_t len, RecWhereFn where,
                   const void *where_ctx, RecLog *out, char *err,
                   size_t errsz) {
    memset(out, 0, sizeof(*out));
    out->chain_ok = true;
    out->chain_break_index = -1;
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
        uint64_t off = (uint64_t)(l.lines[i].ptr - data);
        char pos[160]; /* named only when a message needs it: naming
                          counts lines from the start */
        char lerr[256];
        if (!rec_decode(a, l.lines[i].ptr, l.lines[i].len, &recs[n], lerr,
                        sizeof lerr)) {
            if (where)
                where(where_ctx, data, off, pos, sizeof pos);
            else
                where_line(data, off, pos, sizeof pos);
            snprintf(err, errsz, "%s: %s", pos, lerr);
            return false;
        }
        if (out->chain_ok && strcmp(recs[n].prev, prev_hash) != 0) {
            if (where)
                where(where_ctx, data, off, pos, sizeof pos);
            else
                where_line(data, off, pos, sizeof pos);
            out->chain_ok = false;
            out->chain_break_index = n;
            out->chain_break_off = off;
            snprintf(out->chain_err, sizeof out->chain_err,
                     "hash chain broken at %s (record %s)", pos,
                     recs[n].id ? recs[n].id : "init");
        }
        if (recs[n].type == REC_UNKNOWN && out->unknown_n++ == 0)
            out->unknown_type = recs[n].name;
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

bool rec_op_gone(const char *op) {
    return op && (strcmp(op, "delete") == 0 || strcmp(op, "untrack") == 0);
}

void rec_apply(Arena *a, Lines *cur, const Rec *rec) {
    if (rec_op_gone(rec->op)) {
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
        if (rec_op_gone(rec->op)) {
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
        sb_puts(sb, rec->meta_vals[i]); /* already JSON text */
    }
    sb_putc(sb, '}');
}

bool rec_has_summary(const Rec *end) {
    return end && (end->sum_done || end->sum_decided || end->sum_left);
}

void rec_summary_json(StrBuf *sb, const Rec *end) {
    if (!rec_has_summary(end)) {
        sb_puts(sb, ",\"summary\":null");
        return;
    }
    const char *const key[3] = {"done", "decided", "left"};
    const char *const val[3] = {end->sum_done, end->sum_decided,
                                end->sum_left};
    sb_puts(sb, ",\"summary\":{");
    for (int32_t i = 0; i < 3; i++) {
        sb_printf(sb, "%s\"%s\":", i ? "," : "", key[i]);
        if (val[i])
            json_escape_c(sb, val[i]);
        else
            sb_puts(sb, "null");
    }
    sb_putc(sb, '}');
}

/* JSON's own number grammar, so "1" and "-2.5e3" are numbers and "007",
 * "1." and "0x10" stay strings exactly as typed. */
static bool is_json_number(const char *s) {
    const char *p = s;
    if (*p == '-')
        p++;
    if (*p == '0')
        p++;
    else if (*p >= '1' && *p <= '9')
        while (*p >= '0' && *p <= '9')
            p++;
    else
        return false;
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9'))
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-')
            p++;
        if (!(*p >= '0' && *p <= '9'))
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    return *p == '\0';
}

bool rec_meta_parse(Arena *a, const char *kv, const char **key,
                    const char **val, char *err, size_t errsz) {
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv) {
        snprintf(err, errsz, "--meta expects key=value, e.g. --meta "
                             "ticket=T-12");
        return false;
    }
    for (const char *p = kv; p < eq; p++) {
        char c = *p;
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     c == '_';
        if (!alpha && !(p > kv && c >= '0' && c <= '9')) {
            snprintf(err, errsz, "meta key \"%.*s\" must be an identifier "
                                 "(letters, digits, '_', not starting with a "
                                 "digit)",
                     (int)(eq - kv), kv);
            return false;
        }
    }
    *key = arena_strndup(a, kv, (size_t)(eq - kv));
    const char *v = eq + 1;
    if (is_json_number(v) || strcmp(v, "true") == 0 ||
        strcmp(v, "false") == 0) {
        *val = v;
    } else {
        StrBuf vb;
        sb_init(&vb, a);
        json_escape_c(&vb, v);
        *val = sb_finish(&vb);
    }
    return true;
}
