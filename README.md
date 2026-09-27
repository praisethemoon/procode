# procode

Tools for working alongside AI coding agents:

- **lap** records every edit an agent makes, one commit per edit, each
  with its intent (why) and its behavior (what it makes the code do).
- **kb** is a local knowledge base that agents and people both search and
  cite.
- **coboard** is a board of epics, milestones and tickets that developers
  and agents work from together.
- **Artifacts** are pages an agent publishes for people to read in the
  editor: reports, comparisons, findings.

Each ships as a CLI or an MCP server for agents, plus a VS Code view for
people. The four views come as one extension, **procode**.

## The parts

| part | path | what it is |
|---|---|---|
| **lap** | [cli/lap-cli/](cli/lap-cli/) | The edit recorder. It sits below git and never touches it. C11, no dependencies. [SPEC](cli/lap-cli/SPEC.md) |
| **kb** | [cli/kb-cli/](cli/kb-cli/) | The knowledge base: one store per workspace in `.kb/`, keyword and semantic search, provenance and links. C11. [Contract](specs/index-api.md) |
| **coboard** | [packages/coboard/](packages/coboard/) | The board: an append-only `.coboard/log.jsonl` (commit it) and an MCP server. |
| **artifacts** | [packages/artifacts/](packages/artifacts/) | The `.artifact/` store and its MCP server. [Format](specs/artifacts.md) |
| **kb-js**, **kb-mcp** | [packages/kb-js/](packages/kb-js/), [packages/kb-mcp/](packages/kb-mcp/) | A typed client for the kb CLI, and kb as MCP tools. |
| **procode** | [packages/combined/](packages/combined/) | The VS Code extension: Lap History, Knowledge, the Board and Artifacts, with the coboard, kb and artifacts MCP servers inside. It is built from `packages/*-vscode`. |
| **skills** | [.claude/skills/](.claude/skills/) | How agents work here: `lap`, `tickets` (board, lap and git together) and `artifacts`. The lap skill is also in [cli/lap-cli/skill/](cli/lap-cli/skill/) for other projects. |

## Requirements

- A C11 compiler (clang or gcc), and `make` or CMake 3.16+
- Node.js 18+ with npm
- VS Code 1.101+

Development happens on macOS. CMake is the route on Windows.

## Build and test

From the repository root:

```sh
npm run setup    # npm install, then build both CLIs and every package
npm test         # both CLIs' suites, then every package's tests
```

`npm run build:cli` / `build:packages` and `test:cli` / `test:packages` do
one half. The CLIs also build with CMake (`cmake -B build && cmake --build
build`, tests with `ctest --test-dir build`), or alone with `make` in their
own directory.

Run mutation sweeps only with `HOME` and `TMPDIR` pointing at throwaway
directories: some tests feed shell syntax to code whose job is never to run
it.

## Install

In this order: the extension and its MCP servers run the CLIs from `PATH`.

1. **The CLIs.** On macOS and Linux they go into `/usr/local/bin`
   (`PREFIX=...` to change it):

   ```sh
   sudo make -C cli/lap-cli install
   sudo make -C cli/kb-cli install
   ```

   On Windows, build them with CMake and MSVC, from a Developer PowerShell
   for Visual Studio in the repository root:

   ```powershell
   cmake -B build
   cmake --build build --config Release
   ```

   This gives `build\cli\lap-cli\Release\lap.exe` and
   `build\cli\kb-cli\Release\kb.exe`. Put them on `PATH`, for example with
   `cmake --install build --config Release --prefix C:\tools\procode` and
   then adding `C:\tools\procode\bin` to `PATH`.

2. **The extension**, one `.vsix` for every platform:

   ```sh
   npm run package --workspace combined
   code --install-extension packages/combined/procode-0.1.0.vsix
   ```

   Then reload the VS Code window. If the CLIs are not on `PATH`, point the
   settings **Knowledge › Cli Path** and **Board › Lap Path** at them.

3. **Agents.** VS Code's agent gets the MCP servers on its own. For Claude
   Code, run **procode: Set Up MCP for Claude Code** in a project: it adds
   `coboard`, `kb` and `artifacts` to the project's `.mcp.json` and keeps
   any other servers. After installing a new build, restart Claude Code (or
   run `/mcp`) so it starts the new servers.

4. **Stores.** `lap init`, `kb init` in the project root. The board and
   `.artifact/` are created on first write.

**kb's embedding model** (for semantic search; keyword search works
without it) lives in `~/.kb/models/`, shared by every workspace. kb only
reads it and never downloads anything. The default, gte-modernbert-base, is
converted from its Hugging Face weights by
`cli/kb-cli/tools/modernbert/convert.py` (steps and checksums in
`MODERNBERT.md` beside it). nomic-embed-text-v1.5 can be used as it is:

```sh
mkdir -p ~/.kb/models && curl -fL -o ~/.kb/models/nomic-embed-text-v1.5.Q4_K_M.gguf \
  https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF/resolve/main/nomic-embed-text-v1.5.Q4_K_M.gguf
shasum -a 256 ~/.kb/models/nomic-embed-text-v1.5.Q4_K_M.gguf
# d4e388894e09cf3816e8b0896d81d265b55e7a9fff9ab03fe8bf4ef5e11295ac
```

**Without VS Code**, point any MCP client at the servers directly, started
inside the project:

```json
{
  "mcpServers": {
    "coboard":   { "command": "/path/to/procode/packages/coboard/bin/coboard-mcp" },
    "kb":        { "command": "/path/to/procode/packages/kb-mcp/bin/kb-mcp", "env": { "KB_BIN": "kb" } },
    "artifacts": { "command": "/path/to/procode/packages/artifacts/bin/artifacts-mcp" }
  }
}
```

## How the pieces meet

A ticket's work is a lap session tagged with it:
`lap session start "T-12: fix the parser" --meta ticket=T-12`. The ticket's
tab and `board_sessions` then list that session's commits, grouped by
intent. A commit cites another by its hash (`#fa9cebd`), and the views turn
those into links. The `tickets` skill spells out the whole loop.

## Layout

```
cli/        lap-cli, kb-cli                                     C11, make or CMake
packages/   coboard, artifacts, kb-js, kb-mcp,
            lap-vscode, index-vscode, coboard-vscode,
            artifacts-vscode, combined                          TypeScript, one npm workspace
specs/      the kb contract and the artifact format
```

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Soulaymen Chouri.
