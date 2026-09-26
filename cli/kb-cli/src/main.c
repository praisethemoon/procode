#include "cmd.h"

static const char *USAGE =
    "kb " KB_VERSION " - local knowledge base over docs, source and papers\n"
    "\n"
    "usage: kb <command> [args]\n"
    "\n"
    "commands:\n"
    "  init                       create a store in the current directory\n"
    "  add --title T --collection C [--url U] [--mime M] [--file F]\n"
    "      [--meta <json>] [--meta-file <f>]\n"
    "                             file content you already have; the text\n"
    "                             comes from --file or stdin\n"
    "  ls [--collection C] [--source S-n] [--mime M] [--since <iso>]\n"
    "     [--limit N]\n"
    "  get <D-n> [--include text,chunks,links]\n"
    "  search <query> [--collection a,b] [--mode keyword] [--k 10]\n"
    "         [--expand N] [--source S-n] [--mime M] [--since <iso>]\n"
    "         [--min-score X] [--older-than 90d]\n"
    "                             snippets only; kb chunk fetches a passage\n"
    "  chunk <C-n> [--expand N]   one chunk in full, with its neighbours\n"
    "  collections                names, counts, bytes, oldest fetch\n"
    "  collections rename <old> <new>\n"
    "  collections delete <name> [--with-documents]\n"
    "                             forget a topic; refuses while it holds\n"
    "                             documents unless told to forget them too\n"
    "  stats                      per collection: documents, chunks, bytes\n"
    "  sources [--collection C] [--kind K]\n"
    "                             every source, with its document count\n"
    "  sources show <S-n>         one source, its documents, and every time\n"
    "                             they were fetched\n"
    "  forget <D-n|S-n>           forget a document, or a source and its\n"
    "                             documents; kb compact drops the text\n"
    "\n"
    "provenance:\n"
    "  stale [--older-than 90d] [--collection C] [--limit N]\n"
    "                             documents past the threshold, newest\n"
    "                             sources first\n"
    "  refresh <S-n>              read a file source again; re-index it only\n"
    "                             if its text changed\n"
    "  refresh [--older-than 90d] [--collection C]\n"
    "                             REPORTS what would be refetched; kb has no\n"
    "                             HTTP client and fetches nothing\n"
    "\n"
    "links:\n"
    "  links <D-n>                outgoing and incoming, resolved to rows\n"
    "  links add <D-n> <type> <D-m>\n"
    "                             supersedes | cites | analogue_of |\n"
    "                             implements | see_also\n"
    "  links delete <D-n> <type> <D-m>\n"
    "\n"
    "maintenance:\n"
    "  rebuild                    rebuild index/ from the logs and blobs\n"
    "  reindex                    rechunk after a chunker change; each\n"
    "                             rechunked document takes a FRESH chunk\n"
    "                             id range\n"
    "  compact                    drop blobs no live document references\n"
    "  status [--older-than 90d]  path, counts, disk use\n"
    "\n"
    "global:\n"
    "  --json                     machine-readable output, on every command\n"
    "\n"
    "the store:\n"
    "  .kb/ found by walking up from the current directory, like .git;\n"
    "  kb init makes one here. There is no store outside the workspace.\n";

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs(USAGE, stderr);
        return KB_EXIT_ERR;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("kb %s\n", KB_VERSION);
        return KB_EXIT_OK;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        fputs(USAGE, stdout);
        return KB_EXIT_OK;
    }

    Arena *a = arena_new(1 << 16);
    int32_t argc2 = argc - 2;
    char **argv2 = argv + 2;
    int32_t rc;
    if (strcmp(cmd, "init") == 0)
        rc = cmd_init(a, argc2, argv2);
    else if (strcmp(cmd, "add") == 0)
        rc = cmd_add(a, argc2, argv2);
    else if (strcmp(cmd, "ls") == 0)
        rc = cmd_ls(a, argc2, argv2);
    else if (strcmp(cmd, "get") == 0)
        rc = cmd_get(a, argc2, argv2);
    else if (strcmp(cmd, "search") == 0)
        rc = cmd_search(a, argc2, argv2);
    else if (strcmp(cmd, "chunk") == 0)
        rc = cmd_chunk(a, argc2, argv2);
    else if (strcmp(cmd, "rebuild") == 0)
        rc = cmd_rebuild(a, argc2, argv2);
    else if (strcmp(cmd, "collections") == 0)
        rc = cmd_collections(a, argc2, argv2);
    else if (strcmp(cmd, "status") == 0)
        rc = cmd_status(a, argc2, argv2);
    else if (strcmp(cmd, "stale") == 0)
        rc = cmd_stale(a, argc2, argv2);
    else if (strcmp(cmd, "refresh") == 0)
        rc = cmd_refresh(a, argc2, argv2);
    else if (strcmp(cmd, "sources") == 0)
        rc = cmd_sources(a, argc2, argv2);
    else if (strcmp(cmd, "forget") == 0)
        rc = cmd_forget(a, argc2, argv2);
    else if (strcmp(cmd, "links") == 0)
        rc = cmd_links(a, argc2, argv2);
    else if (strcmp(cmd, "stats") == 0)
        rc = cmd_stats(a, argc2, argv2);
    else if (strcmp(cmd, "reindex") == 0)
        rc = cmd_reindex(a, argc2, argv2);
    else if (strcmp(cmd, "compact") == 0)
        rc = cmd_compact(a, argc2, argv2);
    else {
        bool json = has_flag(argc2, argv2, NULL, "--json");
        err_out(json, "unknown_command", "unknown command \"%s\"", cmd);
        if (!json) {
            fputc('\n', stderr);
            fputs(USAGE, stderr);
        }
        rc = KB_EXIT_ERR;
    }
    arena_free(a);
    return (int)rc;
}
