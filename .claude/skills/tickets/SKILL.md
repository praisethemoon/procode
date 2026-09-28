---
name: tickets
description: The workflow for working a ticket in this repository — coboard board, lap session, git — from picking it up to closing it. Use whenever you start, continue or finish work on a board ticket (T-<n>), or the user asks you to work on tickets.
compatibility: Requires the coboard MCP server, the lap CLI (cli/lap-cli) and git.
metadata:
  version: "3"
---

# tickets — one ticket, start to finish

Three records follow every piece of work, and they point at each other:

- the **board** (coboard MCP: `board_*`) says what and why, and holds the
  summary comment;
- **lap** records every edit with its intent and behavior, in a session
  tagged with the ticket (`board_sessions` lists them on the ticket);
- **git** holds the result, one commit per ticket, its message ending in
  `(T-<n>)`.

The lap skill covers lap itself, including where it is (`~/.procode/bin/lap`
with procode installed, else `lap` on PATH); this is the loop around it.

## The loop

1. **Pick it up.** `board_get T-<n>` — read the description and its
   *Done when*. Then `board_update T-<n> status=doing assignee=claude`.
2. **Start the session.**
   `LAP_USER=claude lap session start "T-<n>: <what>" --meta ticket=T-<n>`.
   A branch folder may be a worktree inside the project (e.g.
   `.claude/worktrees/<name>`); the lap skill's branching reference says
   how to start one and what its refusals mean.
   In a branch folder, or a folder with branches, add `--branch <name>`
   (or set `LAP_BRANCH`) here and on every `lap commit`. The board stays
   one: in a lap branch folder coboard works the parent's board by itself;
   an agent in any other folder sets `COBOARD_DIR` to the project's (in
   VS Code, the **Board › Board Folder** setting does the same). If coboard
   answers `stale_parent`, the branch's parent folder moved: point
   `COBOARD_DIR` at it rather than working the branch's copy.
   One session per ticket. Check `lap session current` first; end a stale
   one only after its edits are committed.
3. **Work, committing edits as you go.** After each change,
   `lap status` — then `lap commit <file> -i "<why>" -b "<what it does>"`.
   When a file has several edits, commit `--edit 1` repeatedly, oldest
   first, and **look at `lap status` (or `git diff -U0`) before writing
   each behavior**, so it lands on the edit it describes. A new file is
   committed in parts: `--lines A-B` creates it with one part, the rest
   follow as edits (`--whole-file` only for a file that is one piece; over
   50 lines lap requires one or the other). `lap commit` prints the new commit's short hash; cite it as
   `#<hash>` when a later commit depends on it. A behavior (or intent) that
   landed on the wrong edit, or says something the edit does not do, is
   corrected with `lap amend <commit> [--branch <name>] -i … -b …` (the
   `--branch` where branches exist, as for commit) — not in a later commit's
   message and not in the board comment.
4. **A change that splits into many fragments** (a function moved, a file
   reworked): give every fragment the **same intent**, saying what the
   whole change is for, and its **own behavior**, saying what that
   fragment does. Never number fragments `(1/n) … (n/n)`. lap cannot follow
   a move; the shared intent is what ties the fragments together.
5. **Test.** Run the suites the change touches, and the combined build's
   check when a manifest, command or MCP server changed
   (`npm run build --workspace combined && node packages/combined/scripts/check.mjs`).
   UI: render it (headless Chrome over the real bundle) in a light and a
   dark theme.
6. **Nothing pending, then end.** `lap status` must show no edits of
   yours before `lap session end`. If you end with fragments pending,
   start a follow-up session tagged with the same ticket and commit them,
   each with its intent and behavior. End with the session's summary —
   `--done`, `--decided`, `--left`, each when it says something (the lap
   skill says what goes in each): it is the short factual record the
   ticket's session review shows first.
7. **git.** Stage by path — never `git add -A` or `.` — including
   `.lap/log/` and `.coboard/log.jsonl`. One commit:
   `git commit -m "<what changed> (T-<n>)"`. **No `Co-Authored-By` or any
   AI attribution line.**
8. **Close it.** `board_comment T-<n>` with: the lap session (as
   `<branch>/S<n>` when the work was in a branch folder) and git hash,
   what changed (as the reader needs it, not a diff), what was decided and
   why, test counts, and anything not verified. It may quote the session's
   summary, and adds what only the board needs: the git hash, the test
   counts, what was not verified. Then
   `board_update T-<n> status=done`. Close a milestone or epic when its
   last ticket closes.

## Rules

- **No ticket ids in code comments.** Comments explain the code; the ticket
  link lives in lap (`--meta ticket=`) and the commit message.
- **New work gets a ticket first** — under the epic it belongs to, with a
  *Done when* — then the loop. A small fix still gets one.
- **Agents do not delete board items.** If the user drops a ticket, say
  which ids they should delete in the editor.
- **Never delete computed paths in shell** (`rm -rf "$X"`). Use the
  scratchpad for experiments and leave it.
- **Bringing a branch folder's work back:** in the parent folder, commit
  its own work, `git merge` the branch, then `lap merge <branch>` — in that
  order. On `git_merge_first`, run the `git merge` you skipped and merge
  again; never use `--copy-from-folder` unless the user asks for it.
- **Merging the parent into a branch is not supported.** Never `git merge`
  the parent (main) into a branch folder to stay current: its changes would
  show as the branch's own pending edits and, if committed, come back to the
  parent as branch work. To take in the parent's newer work, merge the
  branch back, then start a fresh branch.
- **The CLIs are the user's to build.** Do not add tickets for CLI
  packaging or releases unless asked.
- Report failures as failures: a skipped test, an unverified claim, a
  render only done headless — say so in the board comment.
