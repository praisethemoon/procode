#include "help.h"

#include <stdlib.h>
#include <string.h>

#define JSON_FLAG {"--json", NULL, NULL, "answer in JSON"}
#define OLDER_THAN                                                           \
    {"--older-than", "--olderThan", "<age>",                                 \
     "what counts as stale: a count and s, m, h, d or w\n(default 90d)"}
#define END {NULL, NULL, NULL, NULL}

static const HelpFlag F_JSON_ONLY[] = {JSON_FLAG, END};

static const HelpFlag F_ADD[] = {
    {"--title", NULL, "<T>", "the document's title"},
    {"--collection", NULL, "<C>", "the collection it files under"},
    {"--url", NULL, "<U>", "where it came from"},
    {"--mime", NULL, "<M>", "its type, e.g. text/markdown"},
    {"--file", NULL, "<F>", "read the content from a file (default: stdin)"},
    {"--meta", NULL, "<json>", "metadata, a JSON object"},
    {"--meta-file", NULL, "<f>", "metadata read from a file"},
    {"--etag", NULL, "<E>", "the source's etag, for a later refresh"},
    {"--batch", NULL, NULL,
     "many documents in one transaction: one JSON object a line\n"
     "on stdin (title, collection, content; url, mime, meta, etag\n"
     "optional)"},
    {"--dir", NULL, "<D>",
     "file a folder's source and docs, as git sees it; again:\n"
     "only what changed, and files gone are forgotten"},
    {"--no-forget", NULL, NULL, "with --dir: list the files gone instead of forgetting them"},
    {"--embed-budget", NULL, "<S>",
     "embed for at most S seconds (default 20); the rest is\n"
     "searchable by keyword at once and kb embed finishes it"},
    {"--wait", NULL, NULL, "embed until done"},
    JSON_FLAG,
    END};

static const HelpFlag F_LS[] = {
    {"--collection", NULL, "<a,b>", "only these collections"},
    {"--source", NULL, "<S-n>", "only this source's documents"},
    {"--mime", NULL, "<M>", "only this type"},
    {"--since", NULL, "<date>", "fetched on or after this date"},
    {"--q", NULL, "<text>", "title or source location contains this text"},
    {"--meta", NULL, "<json>", "metadata carries every pair of this JSON object"},
    {"--limit", NULL, "<N>", "at most N documents"},
    {"--after", NULL, "<D-n>", "continue after that document, for the next page"},
    {"--reverse", NULL, NULL, "newest first (the store's order backwards)"},
    OLDER_THAN,
    JSON_FLAG,
    END};

static const HelpFlag F_GET[] = {
    {"--include", NULL, "<text,chunks,links>", "also the text, the chunks or the links"},
    OLDER_THAN,
    JSON_FLAG,
    END};

static const HelpFlag F_SEARCH[] = {
    {"--collection", NULL, "<a,b>", "only these collections"},
    {"--mode", NULL, "<keyword|hybrid|semantic>",
     "how to match (hybrid when the store has vectors)"},
    {"--k", NULL, "<N>", "how many results (default 10)"},
    {"--expand", NULL, "<N>", "N neighbouring chunks around each hit"},
    {"--source", NULL, "<S-n>", "only this source's documents"},
    {"--mime", NULL, "<M>", "only this type"},
    {"--since", NULL, "<date>", "fetched on or after this date"},
    {"--meta", NULL, "<json>", "metadata carries every pair of this JSON object"},
    {"--min-score", "--minScore", "<X>", "drop hits scoring below X"},
    OLDER_THAN,
    {"--fusion", NULL, "<score|rrf>",
     "how keyword and semantic lists merge (default score)"},
    {"--rerank", NULL, NULL, "the reranker rescores the top candidates"},
    {"--rerank-depth", NULL, "<N>", "with --rerank: how many it reads, 1-100 (default 10)"},
    {"--rerank-tokens", NULL, "<N>",
     "with --rerank: tokens of each pair it reads, 32-1024\n(default 512)"},
    JSON_FLAG,
    END};

static const HelpFlag F_CHUNK[] = {
    {"--expand", NULL, "<N>", "N neighbouring chunks either side"},
    JSON_FLAG,
    END};

static const HelpFlag F_COLLECTIONS[] = {
    {"--with-documents", NULL, NULL, "on delete: forget its documents too"},
    JSON_FLAG,
    END};

static const HelpFlag F_COLLECTIONS_DELETE[] = {
    {"--with-documents", NULL, NULL,
     "forget its documents too (without it, a collection that\n"
     "holds documents is refused)"},
    JSON_FLAG,
    END};

static const HelpFlag F_SOURCES[] = {
    {"--collection", NULL, "<C>", "only this collection's sources"},
    {"--kind", NULL, "<K>", "only this kind: file, url, dir or inline"},
    {"--status", NULL, "<S>", "only this status"},
    {"--q", NULL, "<text>", "title or location contains this text"},
    JSON_FLAG,
    END};

static const HelpFlag F_STALE[] = {
    OLDER_THAN,
    {"--collection", NULL, "<C>", "only this collection"},
    {"--limit", NULL, "<N>", "at most N documents"},
    JSON_FLAG,
    END};

static const HelpFlag F_REFRESH[] = {
    OLDER_THAN,
    {"--collection", NULL, "<C>", "only this collection"},
    JSON_FLAG,
    END};

static const HelpFlag F_LINKS[] = {
    {"--all", NULL, NULL, "every link in the store, both ends"},
    OLDER_THAN,
    JSON_FLAG,
    END};

static const HelpFlag F_STATUS[] = {OLDER_THAN, JSON_FLAG, END};

const HelpCmd HELP_CMDS[] = {
    {"init", "init [--json]", "create a store in the current directory",
     F_JSON_ONLY, cmd_init, false, "commands"},
    {"add",
     "add --title T --collection C [--url U] [--mime M] [--file F]\n"
     "[--meta <json>] [--meta-file <f>] [--etag E]\n"
     "| add --batch | add --dir D --collection C [--no-forget]\n"
     "[--embed-budget S | --wait] [--json]",
     "file content you already have (from --file or stdin), many\n"
     "documents at once (--batch), or a folder (--dir)",
     F_ADD, cmd_add, false, NULL},
    {"ls",
     "ls [--collection a,b] [--source S-n] [--mime M] [--since <date>]\n"
     "[--q text] [--meta <json>] [--limit N] [--after D-n] [--reverse]\n"
     "[--older-than 90d] [--json]",
     "the documents, in the store's order (oldest first; --reverse for\n"
     "newest first), a page at a time with --limit and --after",
     F_LS, cmd_ls, false, NULL},
    {"get", "get <D-n> [--include text,chunks,links] [--older-than 90d] [--json]",
     "one document", F_GET, cmd_get, false, NULL},
    {"search",
     "search <query> [--collection a,b] [--mode keyword] [--k 10]\n"
     "[--expand N] [--source S-n] [--mime M] [--since <date>]\n"
     "[--meta <json>] [--min-score X] [--older-than 90d]\n"
     "[--fusion score|rrf] [--rerank [--rerank-depth N]\n"
     "[--rerank-tokens N]] [--json]",
     "snippets only; kb chunk fetches a passage", F_SEARCH, cmd_search, false,
     NULL},
    {"chunk", "chunk <C-n> [--expand N] [--json]",
     "one chunk in full, with its neighbours", F_CHUNK, cmd_chunk, false, NULL},
    {"collections", "collections [--json]", "names, counts, bytes, oldest fetch",
     F_COLLECTIONS, cmd_collections, true, NULL},
    {"collections rename", "collections rename <old> <new> [--json]",
     "give a collection another name", F_JSON_ONLY, NULL, false, NULL},
    {"collections delete",
     "collections delete <name> [--with-documents] [--json]",
     "forget a topic; refuses while it holds documents unless told\n"
     "to forget them too",
     F_COLLECTIONS_DELETE, NULL, false, NULL},
    {"stats", "stats [--json]", "per collection: documents, chunks, bytes",
     F_JSON_ONLY, cmd_stats, false, NULL},
    {"sources",
     "sources [--collection C] [--kind K] [--status S] [--q text] [--json]",
     "every source, with its document count", F_SOURCES, cmd_sources, true,
     NULL},
    {"sources show", "sources show <S-n> [--json]",
     "one source, its documents, and every time they were fetched",
     F_JSON_ONLY, NULL, false, NULL},
    {"forget", "forget <D-n|S-n> [--json]",
     "forget a document, or a source and its documents; kb compact\n"
     "drops the text",
     F_JSON_ONLY, cmd_forget, false, NULL},
    {"stale", "stale [--older-than 90d] [--collection C] [--limit N] [--json]",
     "documents past the threshold, newest sources first", F_STALE, cmd_stale,
     false, "provenance"},
    {"refresh",
     "refresh <S-n> [--json]\n"
     "| refresh [--older-than 90d] [--collection C] [--json]",
     "with a file source: read it again and re-index it only if its\n"
     "text changed; without: REPORT what would be refetched (kb has no\n"
     "HTTP client and fetches nothing)",
     F_REFRESH, cmd_refresh, false, NULL},
    {"links", "links (<D-n> | --all) [--older-than 90d] [--json]",
     "a document's links, outgoing and incoming, resolved to rows; or\n"
     "every link in the store",
     F_LINKS, cmd_links, true, "links"},
    {"links add", "links add <D-n> <type> <D-m> [--json]",
     "link two documents: supersedes | cites | analogue_of |\n"
     "implements | see_also (imports links are kb add --dir's)",
     F_JSON_ONLY, NULL, false, NULL},
    {"links delete", "links delete <D-n> <type> <D-m> [--json]",
     "remove a link", F_JSON_ONLY, NULL, false, NULL},
    {"embed", "embed [--json]", "embed the chunks an add left pending",
     F_JSON_ONLY, cmd_embed, false, "maintenance"},
    {"rebuild", "rebuild [--json]", "rebuild index/ from the logs and blobs",
     F_JSON_ONLY, cmd_rebuild, false, NULL},
    {"reindex", "reindex [--json]",
     "rechunk after a chunker change; each rechunked document takes a\n"
     "FRESH chunk id range",
     F_JSON_ONLY, cmd_reindex, false, NULL},
    {"compact", "compact [--json]", "drop blobs no live document references",
     F_JSON_ONLY, cmd_compact, false, NULL},
    {"status", "status [--older-than 90d] [--json]", "path, counts, disk use",
     F_STATUS, cmd_status, false, NULL},
    {NULL, NULL, NULL, NULL, NULL, false, NULL},
};

static const char *FOOTER =
    "global:\n"
    "  --json                     machine-readable output, on every command\n"
    "\n"
    "the store:\n"
    "  .kb/ found by walking up from the current directory, like .git;\n"
    "  kb init makes one here. There is no store outside the workspace.\n";

const HelpCmd *help_running = NULL;

static bool in_group(const HelpCmd *g, const HelpCmd *s) {
    size_t n = strlen(g->name);
    return strncmp(s->name, g->name, n) == 0 && s->name[n] == ' ';
}

const HelpCmd *help_find(const char *name) {
    for (const HelpCmd *c = HELP_CMDS; c->name; c++)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

const HelpCmd *help_sub(const HelpCmd *group, const char *word) {
    if (!group || !group->group || !word)
        return NULL;
    for (const HelpCmd *s = group + 1; s->name && in_group(group, s); s++)
        if (strcmp(s->name + strlen(group->name) + 1, word) == 0)
            return s;
    return NULL;
}

#define MAX_FLAGS 40
#define ENTRIES 64

/* Each entry's lists, built on first use and kept: commands read them
 * through their file's VALUE_FLAGS and BOOL_FLAGS. */
static const char *values_of[ENTRIES][MAX_FLAGS];
static const char *bools_of[ENTRIES][MAX_FLAGS];
static bool built[ENTRIES];

static int32_t entry(const char *name) {
    const HelpCmd *c = help_find(name);
    int32_t i = c ? (int32_t)(c - HELP_CMDS) : -1;
    if (i < 0 || i >= ENTRIES) {
        fprintf(stderr, "kb: internal error: no help entry for %s\n", name);
        abort();
    }
    if (!built[i]) {
        int32_t nv = 0, nb = 0;
        for (const HelpFlag *f = c->flags; f->name; f++) {
            const char **list = f->arg ? values_of[i] : bools_of[i];
            int32_t *n = f->arg ? &nv : &nb;
            if (*n + 3 > MAX_FLAGS)
                abort();
            list[(*n)++] = f->name;
            if (f->alias)
                list[(*n)++] = f->alias;
        }
        values_of[i][nv] = NULL;
        bools_of[i][nb] = NULL;
        built[i] = true;
    }
    return i;
}

const char *const *help_values(const char *name) {
    return values_of[entry(name)];
}

const char *const *help_bools(const char *name) {
    return bools_of[entry(name)];
}

bool help_asked(int32_t argc, char **argv, const char *const *value_flags) {
    return has_flag(argc, argv, value_flags, "--help") ||
           has_flag(argc, argv, value_flags, "-h");
}

static void put_lines(FILE *f, const char *indent, const char *text) {
    fputs(indent, f);
    for (const char *p = text; *p; p++) {
        fputc(*p, f);
        if (*p == '\n' && p[1])
            fputs(indent, f);
    }
    fputc('\n', f);
}

static void put_synopsis(FILE *f, const char *first, const char *more,
                         const HelpCmd *c) {
    fputs(first, f);
    for (const char *p = c->synopsis; *p; p++) {
        fputc(*p, f);
        if (*p == '\n')
            fputs(more, f);
    }
    fputc('\n', f);
}

static void put_flags(FILE *f, const HelpFlag *flags) {
    const int32_t col = 30;
    for (const HelpFlag *x = flags; x->name; x++) {
        char left[96];
        snprintf(left, sizeof left, "%s%s%s%s%s", x->name, x->alias ? ", " : "",
                 x->alias ? x->alias : "", x->arg ? " " : "",
                 x->arg ? x->arg : "");
        fprintf(f, "  %s", left);
        int32_t used = 2 + (int32_t)strlen(left);
        if (used + 2 > col) {
            fputc('\n', f);
            used = 0;
        }
        fprintf(f, "%*s", col - used, "");
        for (const char *p = x->text; *p; p++) {
            fputc(*p, f);
            if (*p == '\n')
                fprintf(f, "%*s", col, "");
        }
        fputc('\n', f);
    }
}

void help_usage(FILE *f) {
    fputs("kb " KB_VERSION " - local knowledge base over docs, source and papers\n"
          "\n"
          "usage: kb <command> [args]\n"
          "       kb <command> --help   (or -h, or kb help <command>): that\n"
          "                             command's flags, one line each\n",
          f);
    for (const HelpCmd *c = HELP_CMDS; c->name; c++) {
        if (c->section)
            fprintf(f, "\n%s:\n", c->section);
        put_synopsis(f, "  ", "      ", c);
        put_lines(f, "        ", c->summary);
    }
    fputc('\n', f);
    fputs(FOOTER, f);
}

void help_command(FILE *f, const HelpCmd *c) {
    put_synopsis(f, "usage: kb ", "          ", c);
    fputc('\n', f);
    put_lines(f, "", c->summary);
    if (c->group) {
        fprintf(f, "\nsubcommands (kb %s <subcommand> --help for one):\n", c->name);
        for (const HelpCmd *s = c + 1; s->name && in_group(c, s); s++) {
            put_synopsis(f, "  ", "      ", s);
            put_lines(f, "        ", s->summary);
        }
    }
    fputs("\nflags:\n", f);
    put_flags(f, c->flags);
}

void help_synopsis(FILE *f, const HelpCmd *c) {
    fputs("usage: kb ", f);
    for (const HelpCmd *s = c; s->name && (s == c || (c->group && in_group(c, s))); s++) {
        if (s != c)
            fputs(" | kb ", f);
        for (const char *p = s->synopsis; *p; p++)
            fputc(*p == '\n' ? ' ' : *p, f);
    }
    fputc('\n', f);
}
