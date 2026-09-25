#include "cmd.h"

static const char *USAGE =
    "kb " KB_VERSION " - local knowledge base over docs, source and papers\n"
    "\n"
    "usage: kb <command> [args]\n"
    "\n"
    "commands:\n"
    "  init [--store project|global]\n"
    "                             create a store here (or the global one)\n"
    "  add --title T --collection C [--url U] [--mime M] [--file F]\n"
    "      [--meta <json>] [--meta-file <f>] [--store project|global]\n"
    "                             file content you already have; the text\n"
    "                             comes from --file or stdin\n"
    "  ls [--collection C] [--source S-n] [--mime M] [--since <iso>]\n"
    "     [--limit N] [--store project|global|all]\n"
    "  get <D-n> [--include text,chunks] [--store project|global|all]\n"
    "  search <query> [--collection a,b] [--mode keyword] [--k 10]\n"
    "         [--expand N] [--store project|global|all] [--source S-n]\n"
    "         [--mime M] [--since <iso>] [--min-score X]\n"
    "                             snippets only; kb chunk fetches a passage\n"
    "  chunk <C-n> [--expand N] [--store project|global|all]\n"
    "                             one chunk in full, with its neighbours\n"
    "  collections [--store project|global|all]\n"
    "  rebuild [--store project|global|all]\n"
    "                             rebuild index/ from the logs and blobs\n"
    "  status                     both tiers: paths, counts, disk use\n"
    "\n"
    "global:\n"
    "  --json                     machine-readable output, on every command\n"
    "\n"
    "stores:\n"
    "  project  .kb/ found by walking up from here, like .git\n"
    "  global   $KB_STORE, else ~/.kb\n"
    "  a write goes to the project store when one exists, else to global;\n"
    "  a read spans both unless --store narrows it\n";

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
