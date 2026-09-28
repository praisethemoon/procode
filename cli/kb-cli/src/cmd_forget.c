#include "cmd.h"
#include "help.h"

/* §2's DELETE /documents/{id} and DELETE /sources/{id}:
 *
 *   kb forget D-n     forget one document
 *   kb forget S-n     forget a source and every document under it
 *
 * FORGETTING IS AN APPEND, like everything else in the logs: a forget record
 * removes the entity from the fold, and its id — and a document's chunk range
 * — stay spent, so nothing is ever handed a forgotten identifier (§1.1). A
 * link that named a forgotten document keeps reading, as `resolved: false`
 * (§6). The text stays in its blob until `kb compact` finds nothing referring
 * to it, so a forget can still be undone by hand from the log and the blob
 * until then.
 *
 * NOT AN MCP TOOL. §9: forgetting is the reader's decision, not an agent's.
 */

/* The flags kb forget accepts: the ones its --help lists (help.c). */
#define VALUE_FLAGS help_values("forget")
#define BOOL_FLAGS help_bools("forget")

int32_t cmd_forget(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *id = positional_arg(argc, argv, VALUE_FLAGS, 0);
    bool is_doc = id && kb_id_num(id, 'D') > 0;
    bool is_src = id && kb_id_num(id, 'S') > 0;
    if (!is_doc && !is_src) {
        err_out(json, "usage", "kb forget expects a document or a source id, "
                               "e.g. D-241 or S-3");
        return KB_EXIT_ERR;
    }
    if (positional_arg(argc, argv, VALUE_FLAGS, 1)) {
        err_out(json, "usage", "kb forget takes one id at a time");
        return KB_EXIT_ERR;
    }

    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }
    Store s;
    const char *code;
    if (!store_open(a, &s, dir, true, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return strcmp(code, "internal") == 0 ? KB_EXIT_FATAL : KB_EXIT_ERR;
    }

    /* Decided from the log this locked open just read. */
    const char **docs = (const char **)arena_alloc(
        a, (s.documents.n ? s.documents.n : 1) * sizeof(char *));
    size_t ndocs = 0;
    const char *src = NULL;
    if (is_doc) {
        const Document *d = doc_by_id(&s.documents, id);
        if (!d) {
            store_close(&s);
            err_out(json, "not_found", "no document %s", id);
            return KB_EXIT_ERR;
        }
        docs[ndocs++] = d->id;
    } else {
        const Source *so = src_by_id(&s.sources, id);
        if (!so) {
            store_close(&s);
            err_out(json, "not_found", "no source %s", id);
            return KB_EXIT_ERR;
        }
        src = so->id;
        for (size_t i = 0; i < s.documents.n; i++) {
            if (strcmp(s.documents.v[i].source, src) == 0)
                docs[ndocs++] = s.documents.v[i].id;
        }
    }

    if (!forget_records(a, &s, docs, ndocs, src ? &src : NULL, src ? 1 : 0,
                        err, sizeof err)) {
        store_close(&s);
        err_out(json, "internal", "%s", err);
        return KB_EXIT_FATAL;
    }

    StrBuf sb;
    sb_init(&sb, a);
    if (json) {
        sb_puts(&sb, "{\"ok\":true,\"documents\":[");
        for (size_t i = 0; i < ndocs; i++)
            sb_printf(&sb, "%s\"%s\"", i ? "," : "", docs[i]);
        sb_puts(&sb, "],\"sources\":[");
        if (src)
            sb_printf(&sb, "\"%s\"", src);
        sb_puts(&sb, "],\"note\":");
        json_escape_c(&sb, "their text stays in the store's blobs until "
                           "kb compact drops what nothing refers to");
        sb_putc(&sb, '}');
        puts(sb_finish(&sb));
    } else {
        if (src)
            sb_printf(&sb, "forgot %s and its %zu document%s", src, ndocs,
                      ndocs == 1 ? "" : "s");
        else
            sb_printf(&sb, "forgot %s", docs[0]);
        for (size_t i = 0; src && i < ndocs; i++)
            sb_printf(&sb, "%s%s", i ? ", " : ": ", docs[i]);
        sb_puts(&sb, "\n(kb compact drops their text once nothing refers "
                     "to it)\n");
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}
