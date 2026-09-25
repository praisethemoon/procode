#include "cmd.h"

/* §6 — a small, optional layer over documents. Not a graph database.
 *
 *   kb link   <from> <type> <to>     POST /links
 *   kb unlink <from> <type> <to>     DELETE /links/{from}/{type}/{to}
 *   kb links  <D-n>                  GET /documents/{id}/links
 *
 * DOCUMENTS ONLY. Entity nodes — an API symbol, a concept, a platform as
 * first-class things — are deliberately absent (§6, §12.1) until there is a
 * traversal retrieval cannot answer, and nothing here invents one. Both ends
 * of an edge are document identifiers and there is no other kind of node.
 *
 * WITHIN ONE STORE. Identifiers are allocated per store (§1.4), so "D-1"
 * names a different document in each tier, and §1.2's Link carries no store
 * field. A cross-tier edge is therefore not representable, and rather than
 * invent a spelling for one, a link is written into the store both of its
 * ends live in. `kb links` resolves an id the way `kb get` does: project
 * first, first match wins.
 *
 * A LINK TO A DOCUMENT THAT DOES NOT EXIST IS REFUSED AT WRITE TIME, and a
 * link whose target is later forgotten still reads — as a row with
 * `resolved: false`. Those two rules are not in tension: the first keeps a
 * typo from becoming a dangling edge, and the second keeps a dangling edge
 * from becoming an unreadable document. `unlink` deliberately does NOT check
 * that either end still exists, because removing an edge whose far end is
 * gone is exactly the repair a reader would want.
 */

static const char *const VALUE_FLAGS[] = {"--store", NULL};
static const char *const BOOL_FLAGS[] = {"--json", NULL};

/* "supersedes, cites, analogue_of, implements or see_also" — built from the
 * one table so a type added there cannot be missing from the message. */
static const char *type_list(Arena *a) {
    StrBuf sb;
    sb_init(&sb, a);
    for (int32_t i = 0; LINK_TYPES[i]; i++) {
        if (i)
            sb_puts(&sb, LINK_TYPES[i + 1] ? ", " : " or ");
        sb_puts(&sb, LINK_TYPES[i]);
    }
    return sb_finish(&sb);
}

int32_t cmd_link(Arena *a, int32_t argc, char **argv, bool remove) {
    bool json = has_flag(argc, argv, VALUE_FLAGS, "--json");
    const char *verb = remove ? "unlink" : "link";
    const char *bad = unknown_flag(argc, argv, VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *from = positional_arg(argc, argv, VALUE_FLAGS, 0);
    const char *rel_in = positional_arg(argc, argv, VALUE_FLAGS, 1);
    const char *to = positional_arg(argc, argv, VALUE_FLAGS, 2);
    if (!from || !rel_in || !to) {
        err_out(json, "usage",
                "kb %s expects <from> <type> <to>, e.g. kb %s D-1 supersedes "
                "D-2",
                verb, verb);
        return KB_EXIT_ERR;
    }
    if (positional_arg(argc, argv, VALUE_FLAGS, 3)) {
        err_out(json, "usage", "kb %s takes exactly three arguments", verb);
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
        err_out(json, "usage",
                "\"%s\" is not a link type; the five are %s", rel_in,
                type_list(a));
        return KB_EXIT_ERR;
    }
    StoreSel sel;
    if (!store_sel_parse(flag_value(argc, argv, VALUE_FLAGS, "--store"),
                         &sel)) {
        err_out(json, "usage", "--store expects project or global");
        return KB_EXIT_ERR;
    }
    if (sel == SEL_ALL) {
        err_out(json, "usage", "--store all cannot be a write target");
        return KB_EXIT_ERR;
    }

    char err[512];
    TierSet tiers;
    if (!tiers_resolve(sel, true, &tiers, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }
    Store s;
    const char *code;
    if (!store_open(a, &s, tiers.dir[0], tiers.tier[0], true, err, sizeof err,
                    &code)) {
        err_out(json, code, "%s", err);
        return strcmp(code, "internal") == 0 ? KB_EXIT_FATAL : KB_EXIT_ERR;
    }

    /* Read-then-write, inside the one locked section: whether the documents
     * exist and whether the edge is already there are both decided from a
     * log nobody else can be appending to. */
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
                    "no document %s in the %s store; a link to a document "
                    "that does not exist is a typo, not an edge",
                    missing, tier_name(s.tier));
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
                  "{\"ok\":true,\"action\":\"%s\",\"store\":\"%s\","
                  "\"from\":\"%s\",\"type\":\"%s\",\"to\":\"%s\","
                  "\"changed\":%s,\"at\":\"%s\"}",
                  verb, tier_name(s.tier), from, rel, to,
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

static const char *const READ_VALUE_FLAGS[] = {"--store", "--older-than",
                                               "--olderThan", NULL};

int32_t cmd_links(Arena *a, int32_t argc, char **argv) {
    bool json = has_flag(argc, argv, READ_VALUE_FLAGS, "--json");
    const char *bad = unknown_flag(argc, argv, READ_VALUE_FLAGS, BOOL_FLAGS);
    if (bad) {
        err_out(json, "usage", "unknown option \"%s\"", bad);
        return KB_EXIT_ERR;
    }
    const char *id = positional_arg(argc, argv, READ_VALUE_FLAGS, 0);
    if (!id || kb_id_num(id, 'D') == 0) {
        err_out(json, "usage",
                "kb links expects a document id, e.g. kb links D-241");
        return KB_EXIT_ERR;
    }
    StoreSel sel;
    if (!store_sel_parse(flag_value(argc, argv, READ_VALUE_FLAGS, "--store"),
                         &sel)) {
        err_out(json, "usage", "--store expects project, global or all");
        return KB_EXIT_ERR;
    }

    char err[512];
    Staleness st;
    if (!staleness_init(&st, older_than_arg(argc, argv, READ_VALUE_FLAGS), err,
                        sizeof err)) {
        err_out(json, "usage", "%s", err);
        return KB_EXIT_ERR;
    }
    TierSet tiers;
    if (!tiers_resolve(sel, false, &tiers, err, sizeof err)) {
        err_out(json, "not_found", "%s", err);
        return KB_EXIT_ERR;
    }

    for (size_t t = 0; t < tiers.n; t++) {
        Store s;
        const char *code;
        if (!store_open(a, &s, tiers.dir[t], tiers.tier[t], false, err,
                        sizeof err, &code)) {
            err_out(json, code, "%s", err);
            return KB_EXIT_ERR;
        }
        if (!doc_by_id(&s.documents, id)) {
            store_close(&s);
            continue;
        }
        if (json) {
            StrBuf sb;
            sb_init(&sb, a);
            sb_printf(&sb, "{\"ok\":true,\"document\":\"%s\",\"store\":\"%s\","
                           "\"outgoing\":",
                      id, tier_name(s.tier));
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
    err_out(json, "not_found", "no document %s", id);
    return KB_EXIT_ERR;
}
