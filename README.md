# procode

A set of tools for working alongside AI coding agents. **lap** records every
small edit an agent makes, with the reason it made it. **kb** is a local
knowledge base that agents and people can both search and cite. **coboard**
is a small board of epics, milestones and tickets that developers and agents
work on together.

## The tools

| tool | path | what it is |
|---|---|---|
| **lap** | [cli/lap-cli/](cli/lap-cli/) | A fine-grained, git-like edit recorder for agents: one commit is one edit with its reason, grouped into sessions. It sits below git and never touches it. C11, no dependencies. |
| **Lap History** | [packages/lap-vscode/](packages/lap-vscode/) | VS Code extension. A live, view-only tree of lap sessions and commits, with each commit shown as a diff. |
| **lap skill** | [cli/lap-cli/skill/lap/](cli/lap-cli/skill/lap/) | Agent skill that teaches the lap workflow. This repository uses its own copy in `.claude/skills/lap/`. |
| **kb** | [cli/kb-cli/](cli/kb-cli/) | A local knowledge base over docs, source and papers, with keyword search, provenance and links. It keeps one store per workspace, `.kb/`, found by walking up like `.git`. C11, no runtime dependencies. |
| **kb-js** | [packages/kb-js/](packages/kb-js/) | Typed TypeScript client for kb. It runs the CLI with an argument array, never a shell, and parses `--json`. |
| **kb-mcp** | [packages/kb-mcp/](packages/kb-mcp/) | kb as MCP tools for agents (search, get, add, collections, links, stale), JSON-RPC 2.0 over stdio. |
| **Knowledge** | [packages/index-vscode/](packages/index-vscode/) | VS Code extension that reads a kb store: search it, read a document, see where it came from. |
| **coboard** | [packages/coboard/](packages/coboard/) | The board: epics (`E-1`) hold milestones (`M-1`); tickets (`T-1`) belong to an epic and optionally one of its milestones. An append-only `.coboard/log.jsonl` (commit it), a generic search, and an MCP server for agents (list, search, get, create, update, move, comment, sessions). |
| **Board** | [packages/coboard-vscode/](packages/coboard-vscode/) | VS Code extension: a tree of the board in the activity bar, and one editor tab per item, rendered with baukasten, with Markdown descriptions and comments, editable in place. |

The kb contract lives in [specs/](specs/): [index-api.md](specs/index-api.md)
for the CLI and store, [index-ui.md](specs/index-ui.md) for the reader. The
name `kb` is provisional.

## Layout

```
cli/        lap-cli, kb-cli                       C11, make or CMake
packages/   kb-js, kb-mcp, index-vscode, lap-vscode,
            coboard, coboard-vscode                   TypeScript, one npm workspace
specs/      the kb contract
```

## Requirements

- A C11 compiler (clang or gcc), with `make` or CMake 3.16 or newer
- Node.js 18 or newer, with npm
- VS Code 1.85 or newer, for the two extensions
- `@vscode/vsce`, only to package the extensions: `npm install -g @vscode/vsce`

Development happens on macOS. CMake is the route for Windows.

## Build

From the repository root:

```sh
npm run setup      # npm install, then build everything
```

| command | does |
|---|---|
| `npm run build` | build both CLIs with `make`, then compile every package |
| `npm run build:cli` | only the CLIs |
| `npm run build:packages` | only the TypeScript packages |
| `npm run clean` | `make clean` in both CLIs |

The CLIs also build with CMake from the root, which is the path for Windows and
for IDEs:

```sh
cmake -B build && cmake --build build
```

Each CLI builds on its own too: `make` inside `cli/lap-cli` or `cli/kb-cli`.

## Install

**lap and kb.** Each CLI's Makefile installs its binary into `$(PREFIX)/bin`,
which defaults to `/usr/local/bin`:

```sh
sudo make -C cli/lap-cli install
sudo make -C cli/kb-cli install
# elsewhere: PREFIX=$HOME/.local make -C cli/kb-cli install
# remove:    sudo make -C cli/kb-cli uninstall
```

With CMake, `cmake --install build` installs both, and
`cmake --build build --target uninstall` removes them.

**kb-mcp.** Point your MCP client at `packages/kb-mcp/bin/kb-mcp`. It needs
Node and a built `kb`. `KB_BIN` names the `kb` executable, and a bare name is
resolved through `PATH`:

```json
{
  "mcpServers": {
    "kb": {
      "command": "/path/to/procode/packages/kb-mcp/bin/kb-mcp",
      "env": { "KB_BIN": "kb" }
    }
  }
}
```

Run `kb init` at a project's root to give agents a store to file into.

Semantic search (still being wired in) needs one embedding model, shared by all
workspaces, in `~/.kb/models/`. kb only reads it and never downloads anything:

```sh
mkdir -p ~/.kb/models && curl -fL -o ~/.kb/models/nomic-embed-text-v1.5.Q4_K_M.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf
shasum -a 256 ~/.kb/models/nomic-embed-text-v1.5.Q4_K_M.gguf
# d4e388894e09cf3816e8b0896d81d265b55e7a9fff9ab03fe8bf4ef5e11295ac
``` The
server's `kb` finds it by walking up from its working directory, so start the
MCP server from inside the project; with no `.kb/` above it, every tool refuses
with `not_found` rather than filing anywhere else.

**Knowledge (VS Code).** `npm run package` builds a self-contained
`packages/index-vscode/index-vscode-0.1.0.vsix`. Install it with
`code --install-extension <file>.vsix`. The setting `knowledge.cliPath`
(default `kb`) tells it where the CLI is.

**coboard (MCP).** Point your MCP client at `packages/coboard/bin/coboard-mcp`,
started inside the project. It uses the `.coboard/` above its working directory;
with none, the first write creates one at the git repository's root.

```json
{ "mcpServers": { "coboard": { "command": "/path/to/procode/packages/coboard/bin/coboard-mcp" } } }
```

Work on a ticket is linked through lap session metadata: start the session with
`lap session start "T-12: fix the parser" --meta ticket=T-12`, and the ticket's
tab (and `board_sessions`) lists that session and its commits. `COBOARD_AUTHOR`
(else `LAP_USER`) signs an agent's comments.

**Board (VS Code).** `npm run package --workspace coboard-vscode` builds a
self-contained `.vsix`; install it like the others. The Board icon in the
activity bar opens the tree; clicking an item opens its tab, where ids in
Markdown link to other items.

**Lap History (VS Code).** Package it with `npx @vscode/vsce package` inside
`packages/lap-vscode`, then install the `.vsix` the same way. It reads
`.lap/` directly and does not need the CLI at runtime.

**lap skill.** Copy `cli/lap-cli/skill/lap/` into a project's
`.claude/skills/`, or into `~/.claude/skills/` for every project.

## Test

| command | does |
|---|---|
| `npm test` | both CLIs' `make test`, then every package's tests |
| `npm run test:cli` | only the CLIs |
| `npm run test:packages` | only the TypeScript packages |
| `ctest --test-dir build` | the CLI suites through CMake: `lap.unit`, `lap.e2e`, `kb.unit`, `kb.e2e` |

Package tests always recompile first. Run mutation sweeps only in a sandbox,
with `HOME` and `TMPDIR` pointing at throwaway directories: some tests feed
shell syntax to code whose whole job is never to run it.

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Soulaymen Chouri.
