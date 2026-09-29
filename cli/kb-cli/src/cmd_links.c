#include "cmd.h"
#include "help.h"

/* §6 — a small, optional layer over documents. Not a graph database.
 *
 *   kb links <D-n>                       GET /documents/{id}/links
 *   kb links add    <from> <type> <to>   POST /links
 *   kb links delete <from> <type> <to>   DELETE /links/{from}/{type}/{to}
 *
 * The verbs are subcommands of the noun, exactly as `kb collections rename`
 * and `kb collections delete` are, and each is named after its route's
 * method. A top-level `kb link` beside `kb links` would be two commands one
 * letter apart that do opposite things.
 *
 * DOCUMENTS ONLY. Entity nodes — an API symbol, a concept, a platform as
 * first-class things — are deliberately absent (§6, §12.1) until there is a
 * traversal retrieval cannot answer, and nothing here invents one. Both ends
 * of an edge are document identifiers and there is no other kind of node.
 *
 * A LINK TO A DOCUMENT THAT DOES NOT EXIST IS REFUSED AT WRITE TIME, and a
 * link whose target is later forgotten still reads — as a row with
 * `resolved: false`. Those two rules are not in tension: the first keeps a
 * typo from becoming a dangling edge, and the second keeps a dangling edge
 * from becoming an unreadable document. `delete` deliberately does NOT check
 * that either end still exists, because removing an edge whose far end is
 * gone is exactly the repair a reader would want.
 */

/* The flags kb links accepts: the ones its --help lists (help.c). */
#define VALUE_FLAGS help_values("links")
#define BOOL_FLAGS help_bools("links")

/* "supersedes, cites, analogue_of, implements or see_also" — built from the
 * one table so a type added there cannot be missing from the message, and
 * without `imports`, which is kb's to write and not the reader's. */
static const char *type_list(Arena *a) {
    const char *hand[LINK_TYPE_COUNT];
    int32_t n = 0;
    for (int32_t i = 0; LINK_TYPES[i]; i++)
        if (strcmp(LINK_TYPES[i], LINK_IMPORTS) != 0)
            hand[n++] = LINK_TYPES[i];
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 0; i < n; i++) {
        if (i)
            sb_puts(&sb, i + 1 < n ? ", " : " or ");
        sb_puts(&sb, hand[i]);
    }
    return sb_finish(&sb);
}

/* ---- POST /links, DELETE /links/{from}/{type}/{to} --------------------- */

static int32_t links_write(Arena *a, int32_t argc, char **argv, bool json,
                           bool remove) {
    const char *verb = remove ? "delete" : "add";
    const char *from = positional_arg(argc, argv, VALUE_FLAGS, 1);
    const char *rel_in = positional_arg(argc, argv, VALUE_FLAGS, 2);
    const char *to = positional_arg(argc, argv, VALUE_FLAGS, 3);
    if (!from || !rel_in || !to) {
        err_out(json, "usage",
                "kb links %s expects <from> <type> <to>, e.g. kb links %s "
                "D-1 supersedes D-2",
                verb, verb);
        return KB_EXIT_ERR;
    }
    if (positional_arg(argc, argv, VALUE_FLAGS, 4)) {
        err_out(json, "usage", "kb links %s takes exactly three arguments",
                verb);
        return KB_EXIT_ERR;
    }
    if (kb_id_num(from, 'D') == 0 || kb_id_num(to, 'D') == 0) {
        err_out(json, "usage",
                "a link joins two documents; both ends must be D-<n>");
        return KB_EXIT_ERR;
    }
    if (strcmp(from, to) == 0) {
        /* None of §6's five says anything about a document and itself:
         * superseding, citing, being an analogue of or implementing oneself
         * are all statements with no content. */
        err_out(json, "usage", "a document cannot link to itself");
        return KB_EXIT_ERR;
    }
    const char *rel = link_type_canon(rel_in);
    if (!rel) {
        err_out(json, "usage", "\"%s\" is not a link type; the types are %s",
                rel_in, type_list(a));
        return KB_EXIT_ERR;
    }
    if (strcmp(rel, LINK_IMPORTS) == 0) {
        /* An imports link says what a file's code says, and kb add --dir
         * keeps it so; one made or removed by hand would be undone by the
         * next filing of the folder. */
        err_out(json, "usage",
                "imports links are kept by kb add --dir from the files' code, "
                "not made or removed by hand");
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

    /* Read-then-write, inside the one locked section: whether the documents
     * exist and whether the edge is already there are both decided from a log
     * nobody else can be appending to. */
    bool present = link_find(&s.documents, from, rel, to) != NULL;
    if (!remove) {
        const char *missing = NULL;
        if (!doc_by_id(&s.documents, from))
            missing = from;
        else if (!doc_by_id(&s.documents, to))
            missing = to;
        if (missing) {
            store_close(&s);
            err_out(json, "not_found",
                    "no document %s; a link to a document that does not "
                    "exist is a typo, not an edge",
                    missing);
            return KB_EXIT_ERR;
        }
    } else if (!present) {
        store_close(&s);
        err_out(json, "not_found", "no %s link from %s to %s", rel, from, to);
        return KB_EXIT_ERR;
    }

    char now[32];
    plat_timestamp(now);
    bool changed = remove ? present : !present;
    if (changed) {
        Link l;
        l.from = from;
        l.rel = rel;
        l.to = to;
        l.created_at = now;
        size_t len;
        char *line = doc_encode_link(a, &l, !remove, &len);
        if (!store_append(&s, STORE_DOCUMENTS, line, len, err, sizeof err)) {
            store_close(&s);
            err_out(json, "internal", "%s", err);
            return KB_EXIT_FATAL;
        }
    }
    /* No index rebuild. The keyword index is over chunk text, and an edge
     * changes no chunk; fts_store_digest deliberately covers only what
     * decides a chunk's text and identity, so a link cannot stale it. */

    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        /* One shape for both verbs and both outcomes: `action` says what was
         * asked and `changed` says whether the log moved, so a caller never
         * has to work out which keys came back before it can read the
         * answer. */
        sb_printf(&sb,
                  "{\"ok\":true,\"action\":\"%s\",\"from\":\"%s\","
                  "\"type\":\"%s\",\"to\":\"%s\",\"changed\":%s,"
                  "\"at\":\"%s\"}",
                  verb, from, rel, to,
                  changed ? "true" : "false", now);
        puts(sb_finish(&sb));
    } else {
        printf("%s %s %s  %s\n", from, rel, to,
               changed ? (remove ? "(removed)" : "(linked)")
                       : "(already linked)");
    }
    store_close(&s);
    return KB_EXIT_OK;
}

/* ---- GET /documents/{id}/links ----------------------------------------- */

static int32_t links_read(Arena *a, int32_t argc, char **argv, bool json,
                          const char *id) {
    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, VALUE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    Store s;
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }
    if (!doc_by_id(&s.documents, id)) {
        store_close(&s);
        err_out(json, "not_found", "no document %s", id);
        return KB_EXIT_ERR;
    }
    if (json) {
        StrBuf sb;
        sb_init(&sb, a);
        sb_printf(&sb, "{\"ok\":true,\"document\":\"%s\",\"outgoing\":", id);
        json_links(&sb, &s, id, true, &st);
        sb_puts(&sb, ",\"incoming\":");
        json_links(&sb, &s, id, false, &st);
        sb_putc(&sb, '}');
        puts(sb_finish(&sb));
    } else {
        StrBuf sb;
        sb_init(&sb, a);
        size_t shown = 0;
        for (size_t i = 0; i < s.documents.nlinks; i++) {
            const Link *l = &s.documents.links[i];
            bool out = strcmp(l->from, id) == 0;
            if (!out && strcmp(l->to, id) != 0)
                continue;
            const char *far = out ? l->to : l->from;
            const Document *fd = doc_by_id(&s.documents, far);
            sb_printf(&sb, "%s %-11s %-8s ", out ? "->" : "<-", l->rel,
                      far);
            if (fd)
                sb_puts_safe(&sb, fd->title ? fd->title : "");
            else
                sb_puts(&sb, "(forgotten)");
            sb_putc(&sb, '\n');
            shown++;
        }
        if (shown == 0)
            printf("%s has no links\n", id);
        else
            fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}

/* Every live link in the store, for a graph: one row per edge with both ends
 * and whether each end is still a document, and nothing else — the documents
 * themselves are `kb ls`, read once, rather than repeated on every edge. */
static int32_t links_all(Arena *a, bool json) {
    char err[512];
    char dir[KB_PATH_MAX];
    if (!store_resolve(dir, sizeof dir, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }
    Store s;
    const char *code;
    if (!store_open(a, &s, dir, false, err, sizeof err, &code)) {
        err_out(json, code, "%s", err);
        return KB_EXIT_ERR;
    }
    StrBuf sb;
    sb_init(&sb, a);
    if (json)
        sb_puts(&sb, "{\"ok\":true,\"links\":[");
    for (size_t i = 0; i < s.documents.nlinks; i++) {
        const Link *l = &s.documents.links[i];
        bool from_ok = doc_by_id(&s.documents, l->from) != NULL;
        bool to_ok = doc_by_id(&s.documents, l->to) != NULL;
        if (json)
            sb_printf(&sb,
                      "%s{\"type\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                      "\"resolved\":%s}",
                      i ? "," : "", l->rel, l->from, l->to,
                      from_ok && to_ok ? "true" : "false");
        else
            sb_printf(&sb, "%-8s %-11s %-8s%s\n", l->from, l->rel, l->to,
                      from_ok && to_ok ? "" : "  (an end is forgotten)");
    }
    if (json) {
        sb_printf(&sb, "],\"count\":%zu}", s.documents.nlinks);
        puts(sb_finish(&sb));
    } else if (s.documents.nlinks == 0) {
        puts("no links");
    } else {
        fputs(sb_finish(&sb), stdout);
    }
    store_close(&s);
    return KB_EXIT_OK;
}

int32_t cmd_links(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    /* The first positional is either a verb or the document being read. The
     * two can never be confused: a document id is `D-<n>` and no verb is. */
    const char *first = positional_arg(argc, argv, VALUE_FLAGS, 0);
    if (has_flag(argc, argv, VALUE_FLAGS, "--all")) {
        if (first) {
            err_out(json, "usage", "kb links --all takes no document");
            return KB_EXIT_ERR;
        }
        return links_all(a, json);
    }
    if (first && strcmp(first, "add") == 0)
        return links_write(a, argc, argv, json, false);
    if (first && strcmp(first, "delete") == 0)
        return links_write(a, argc, argv, json, true);
    if (!first || kb_id_num(first, 'D') == 0) {
        err_out(json, "usage",
                "kb links expects a document id, or \"add\"/\"delete\" with "
                "<from> <type> <to>");
        return KB_EXIT_ERR;
    }
    if (positional_arg(argc, argv, VALUE_FLAGS, 1)) {
        err_out(json, "usage", "kb links reads one document at a time");
        return KB_EXIT_ERR;
    }
    return links_read(a, argc, argv, json, first);
}
