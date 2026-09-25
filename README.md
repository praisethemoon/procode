<img src="media/lap.svg" alt="lap" width="96" align="right">

# lap — workspace

**A flight recorder for AI agent work sessions**: every small edit
recorded with the reason it exists, below git, without touching git. Beside it
lives **kb**, a local knowledge base agents and readers can search.

## lap

- **[lap-cli/](lap-cli/)** — the lap CLI: a fine-grained, git-like edit
  recorder for AI agents. C11, zero dependencies. Build with `make`, test
  with `make test`. Start with [lap-cli/README.md](lap-cli/README.md) and
  [lap-cli/SPEC.md](lap-cli/SPEC.md).
- **[lap-vscode/](lap-vscode/)** — VSCode extension: live, view-only tree of
  sessions and commits (grouped or raw), commit detail as diff-highlighted
  documents, active-session status bar. Its own `.lap/` holds the history of
  it being built — lap dogfooding lap.
- **.claude/skills/lap/** — agent skill teaching the lap workflow
  (canonical copy lives at `lap-cli/skill/lap/SKILL.md`).

## kb

A knowledge base of documents with full-text and hybrid search. The name is
provisional. The contract is in [specs/](specs/):
[index-api.md](specs/index-api.md) for the CLI and store,
[index-ui.md](specs/index-ui.md) for the reader.

- **[kb-cli/](kb-cli/)** — the `kb` CLI and its on-disk store. C11, zero
  runtime dependencies. `make` builds `bin/kb`; `make unit`, `make e2e` or
  `make test` run the suites.
- **[kb-js/](kb-js/)** — typed reader for kb. It runs the CLI with an
  argument array and never a shell, then parses `--json`. `npm test`.
- **[kb-mcp/](kb-mcp/)** — kb as MCP tools for agents, JSON-RPC 2.0 over
  stdio. `npm test`.
- **[index-vscode/](index-vscode/)** — VSCode extension: find a document, read
  it, see where it came from. `npm run package` builds the `.vsix`.

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Soulaymen Chouri.
