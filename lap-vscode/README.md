# Lap History — VSCode extension

Visualization for [lap](../lap-cli/) repositories: a live tree of sessions
and commits, each with its message, file, and edit range. **View-only by
design** — agents drive lap through the CLI (see the lap skill); this
extension just shows what they did, as they do it.

## What you get

- A **Lap** view in the activity bar:
  - **Grouped by session** (default): sessions newest-first — id, purpose,
    commit count, ● active marker — expandable into their commits; commits
    recorded with `--no-session` sit in their own group. The toggle button
    switches to a **raw list** of all commits, newest-first.
  - Every commit shows `L<n>`, the message's first line, the file and edit
    range; hover for the full message and metadata.
  - Click a commit → the built-in (Monaco) **diff editor** opens directly
    on the file replayed to before vs. after that commit — full syntax
    highlighting, word-level diffs — with the cursor on the changed region,
    and the commit's **description attached inside the diff** as an inline
    comment thread anchored at those lines (id, op, timestamp, message,
    session). Real editors render all code; nothing is hand-drawn.
- A **status bar item** showing the active session (click to focus the view).
- **Live updates**: a watcher on `.lap/log.jsonl` refreshes the tree as the
  agent commits; a torn in-progress log line is tolerated silently.

The log is parsed directly (append-only JSONL, schema in
[lap-cli/SPEC.md](../lap-cli/SPEC.md)) — the extension does not spawn the
CLI and works even where the `lap` binary is not installed.

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
