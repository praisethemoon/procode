# Lap History — VSCode extension

Visualization for [lap](../../cli/lap-cli/) repositories: a live tree of sessions
and commits, each with its intent and behavior, hash, file, and edit range.
**View-only by design** — agents drive lap through the CLI (see the lap
skill); this extension just shows what they did, as they do it.

## What you get

- A **Lap** view in the activity bar:
  - **Grouped by session** (default): sessions newest-first — id, purpose,
    commit count, ● active marker — expandable into their commits; commits
    recorded with `--no-session` sit in their own group. The toggle button
    switches to a **raw list** of all commits, newest-first.
  - Every commit shows `L<n>`, its short hash, the intent's first line, the
    file and edit range. Open it (its twistie, or a click) for the whole
    intent, then the behavior, and a mark when the message checks were
    skipped (`--force-message`). References to other commits in that text —
    `#<hash prefix>` or `L<n>` — are links that reveal the commit in the
    tree. The filter matches ids, hashes, intents, behaviors
    and file paths.
  - Click a commit → the built-in (Monaco) **diff editor** opens directly
    on the file replayed to before vs. after that commit — full syntax
    highlighting, word-level diffs — with the cursor on the changed region,
    and the commit's **description attached inside the diff** as an inline
    comment thread anchored at those lines (id, short hash, timestamp,
    intent, behavior, session), its references linked the same way. Real
    editors render all code; nothing is hand-drawn.
- A **status bar item** showing the active session (click to focus the view).
- **Live updates**: a watcher on `.lap/log.jsonl` refreshes the tree as the
  agent commits; a torn in-progress log line is tolerated silently.

The log is parsed directly (append-only JSONL, schema in
[cli/lap-cli/SPEC.md](../../cli/lap-cli/SPEC.md)). The CLI is run only to resolve a
followed reference (`lap show <ref> --json`); where `lap` is not installed or
cannot answer, the log's own hashes resolve it the same way.

## Develop

```sh
npm install
npm run compile     # or: npm run watch
npm test            # parser unit tests (plain node --test, no VSCode host)
```

Press **F5** in VSCode (Run Extension) to launch an Extension Development
Host on this folder — which is itself a lap repository, recorded while the
extension was built, so the view has real data on first launch.

## Notes / limits

- The first workspace folder containing `.lap/log.jsonl` is shown;
  multi-repo workspaces show only that first one (future work).
- Visualization-only: no commit/session actions from the UI, on purpose.
