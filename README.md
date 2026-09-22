<img src="media/lap.svg" alt="lap" width="96" align="right">

# lap — workspace

**A flight recorder for AI agent work sessions**: every small edit
recorded with the reason it exists, below git, without touching git.

- **[lap-cli/](lap-cli/)** — the lap CLI: a fine-grained, git-like edit
  recorder for AI agents. C11, zero dependencies. Build with `make`, test
  with `make test`. Start with [lap-cli/README.md](lap-cli/README.md) and
  [lap-cli/SPEC.md](lap-cli/SPEC.md).
- **[lap-vscode/](lap-vscode/)** — VSCode extension: live, view-only tree of
  sessions and commits (grouped or raw), commit detail as diff-highlighted
  documents, active-session status bar. Its own `.lap/` holds the history of
  it being built — lap dogfooding lap.
- **[feedback.md](feedback.md)** — dogfooding notes and open questions from
  building the extension with the lap CLI.
- **.claude/skills/lap/** — agent skill teaching the lap workflow
  (canonical copy lives at `lap-cli/skill/lap/SKILL.md`).

## License

MIT — see [LICENSE](LICENSE). Copyright (c) 2026 Soulaymen Chouri.
