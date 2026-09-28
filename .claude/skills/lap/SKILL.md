---
name: lap
description: Record fine-grained edit history with the lap CLI while coding. Use whenever a lap repository (.lap directory) exists in the project, or the user asks to track edits with lap. Teaches the session -> edit -> commit cadence, one-edit-per-commit recovery, and history search.
---

# lap — fine-grained edit recording

lap is a flight recorder for your work: every small edit gets committed with
its intent and what it does, grouped into purposeful sessions. It is
independent of git and never modifies files.

## Identity

Commits record who made them. If you are an agent, set `LAP_USER` so your
commits are attributable (e.g. `LAP_USER="claude/bugfix-agent"`); otherwise
lap falls back to `git config user.name`, then the OS username.

## Cadence (follow this strictly)

1. **Before starting a task**: `lap session start "<task purpose>"` — one
   session per coherent task (a bug fix, a feature slice). If a session is
   already active (`lap session current`), decide: continue it if the task
   is the same, otherwise `lap session end` first.
2. **After every file edit you make**: commit it immediately.
   ```
   lap commit <file> -i "<why this edit exists>" -b "<what it makes the code do>"
   ```
   It prints the new commit's id and short hash: `[L42 fa9cebd] ...`.
   One edit tool call ≈ one lap commit. Do not batch several unrelated
   edits and commit later — that is exactly what lap rejects.
3. **When the task is done**: `lap session end`.

## Commit messages: intent and behavior

Every commit has two fields, both required:

- **intent** (`-i`) — *why* the edit exists: the goal it serves, written as
  the goal, not the mechanics. Edits that serve one goal **share** an
  intent, word for word.
- **behavior** (`-b`) — *what this edit makes the code do*. Every commit
  has its own; a hunk that cannot say what it does is a hunk to rethink.

A goal that takes an import and its use is two commits with one intent:

```
lap commit src/App.tsx -i "Route the graph place to its view." \
  -b "Render Graph when the target place is graph; every other place still gets Collections."
  # -> [L42 fa9cebd] ...
lap commit src/App.tsx -i "Route the graph place to its view." \
  -b "Import Graph for the graph route in #fa9cebd."
```

- **Cite another commit by its hash**, as `#` plus the short hash that
  `lap commit` printed (`#fa9cebd`). Words around it are yours ("needs",
  "fixes", "reverts"); readers follow it with `lap show fa9cebd`, and the
  editor views turn it into a link.
- **Never number fragments** (`(1/3) … (3/3)`, `(n/m)`). A shared intent
  is what ties fragments together; each fragment's behavior says what that
  piece does.
- Multiline is welcome in either field; the first line is its summary.
- For long text, avoid shell quoting by writing both fields to a file (or
  stdin) with `-F`: a line `Intent:`, its text, a line `Behavior:`, its
  text (either order, each once):
  ```
  printf 'Intent:\nRoute the graph place to its view.\n\nBehavior:\nImport Graph for the graph route in #fa9cebd.\n' \
    | lap commit src/App.tsx -F -
  ```
  (`-F` and `-i`/`-b` are mutually exclusive.)

### When lap refuses the message

`lap commit` checks the message before writing anything:

| error | meaning | fix |
|---|---|---|
| `missing_intent` / `missing_behavior` | a field was not given | give both `-i` and `-b` |
| `message_too_short` | intent or behavior has fewer than 3 words | say it in a sentence |
| `behavior_repeats_intent` | the behavior is (nearly) the intent's words | say what *this edit* does, not why |
| `behavior_repeats_previous` | the behavior is (nearly) the session's previous one | say what this edit does that the last one did not |
| `behavior_restates_code` | the behavior is (nearly) the changed lines' words | say what the lines *do*, not what they say |
| `bad_message_file` | the `-F` file is not two `Intent:`/`Behavior:` sections | fix the file |

`--force-message` skips the three repetition checks (never the length
check) and marks the commit `forced`, which `lap show` and `lap rr` print.
Use it only when a near-duplicate is honestly the right description —
e.g. the same one-line fix applied to two copies of a function — never to
get past a message you could have written better.

Unknown flags are refused (`unknown_flag`), naming the flag.

`--dry-run` checks a commit without making it: it picks the edit, runs the
message checks and prints what would be recorded, failing exactly as the
commit would, and writes nothing. Use it to test a message, to see which
edit `--edit N` takes, or before a batch of baseline commits.

### A message written wrong: `lap amend`

When a commit's intent or behavior turns out wrong (it describes another
fragment, it claims what the edit does not do), **correct it with
`lap amend`** — never in a later commit's message (which describes only
its own edit) or in a board comment:

```
lap amend fa9cebd -i "<the intent, repeated if it was right>" -b "<what the edit really does>"
```

Where branches exist (this folder is a branch, or has branches of its
own), add `--branch <name>` as for `lap commit`: this folder's branch, or
`main` in the folder the branches started from; without it lap refuses
with `branch_required`.

Both fields are always given, and the commit message checks apply. It
appends a correction and changes nothing already written; the commit then
shows the new text everywhere, marked "amended", and `lap show` keeps the
earlier ones. The code is not touched: a wrong *edit* is fixed by a new
commit. Only this folder's own commits can be amended (in a branch folder,
not those from before its base: `not_own_commit`); `lap merge` carries a
branch's amendments to the parent.

## When a commit is rejected: "N separate edits detected"

You changed more than one place in the file, with at least one non-blank
unchanged line between the places (gaps of only blank lines never split an
edit — a rewrite around blank lines is one commit). lap lists the edits,
numbered, with line ranges. Commit them one at a time, oldest first, each
with its own behavior (and the shared intent, if they serve one goal):

```
lap commit src/foo.c -i "<goal>" -b "<what the first edit does>" --edit 1
lap commit src/foo.c -i "<goal>" -b "<what the second edit does>" --edit 1   # renumbered!
```

After each commit the remaining edits are re-detected and renumbered — run
`lap status` (or read the fresh error listing) rather than reusing old
numbers or old line ranges. `--lines A-B` also works and must exactly match
a listed range.

## Other situations

- `"no active session"` → start one, or use `--no-session` only for
  genuinely task-independent commits (e.g. committing `.lapignore` itself).
- New files commit whole as one edit (`--lines`/`--edit` are refused on a
  new or deleted file: there is nothing to pick) — but a whole-file commit is only as
  interpretable as its message. When creating a **large** file, prefer
  building it in meaningful increments: write the skeleton, commit it,
  then add each section with its own commit. Reserve one-shot
  creation for small files or content with a single clear purpose.
- Deleting a file is also a commit:
  `rm` it, then `lap commit <file> -i "why it is gone" -b "what no longer exists"`.
- **Renaming is two commits**, because lap tracks paths, not file identity.
  Record the old path's disappearance and the new path's appearance, and
  make each behavior name the other path:
  ```
  mv src/old.c src/new.c
  lap commit src/old.c -i "Name the module after what it does." -b "Removes src/old.c; its content moved to src/new.c."
  lap commit src/new.c -i "Name the module after what it does." -b "Adds src/new.c, renamed from src/old.c with no content changes."
  ```
  Blame and `lap log --file` cannot follow a rename, but those messages
  make the trail findable with `lap search --msg "old.c"`. You know it is a
  rename; the history only knows if you say so. If the content changed in
  transit, say what changed instead of hiding it behind "renamed".
- **`unreadable` in `lap status`**: a file lap cannot read ("cannot be read:
  fix its permissions"), or a folder it cannot open (`unreadable  d/`,
  whose contents are unknown). It is never shown as deleted or clean, and
  `lap commit` refuses it (`unreadable`). Fix the permissions, or ask the
  user — never commit around it.
- Binary files are not tracked; add noisy artifacts to `.lapignore`.
  `.lapignore` keeps lap from recording files it has not recorded yet. A
  file recorded before it was ignored stays tracked: its edits still show
  in `lap status`, so none is lost unseen. To shed tracked files for good,
  start a new history (a fresh `.lap/` from `lap init`): rare, and blunt,
  since the whole history goes with them.
- `lap status --json`, and `--json` on every command, when you want to
  parse output.
- Output is coloured only at a terminal, so piped output is plain. If you
  run lap through a pseudo-terminal, pass `--color=never` (or set
  `NO_COLOR=1`) to keep escape sequences out of what you read back.

## Branches: parallel work in other folders

Two agents in one folder see each other's edits as their own. Parallel
work goes in **branch folders**: a git worktree (or a plain copy) of the
project, made a branch of the first folder:

```
git worktree add ../proj-parser -b parser     # the folder: git's job
cd ../proj-parser
lap branch start parser --from ../proj         # its own line of history
```

The branch folder may also live inside the project (a tool's
`.claude/worktrees/<name>`, `git worktree add sub`): a subfolder with its
own `.lap/` is another repository, so its files never show in the
parent's `lap status`, and a commit of one from the parent is refused
("outside this repository: sub has its own .lap/"). The same rule means a
**leftover `.lap/`** in a subfolder hides that whole folder from lap: if
its files never show in `lap status`, or a commit gives that message for
a folder that is no branch, tell the user — never delete a `.lap/`
yourself.

- The parent's work must be committed (lap and git) first: a branch starts
  from the parent's committed state, and `branch start` refuses files that
  differ (`not_clean`).
- **`branch start` runs in the new folder**, with `--from` naming the
  parent. `has_branches` ("a folder with branches cannot become a branch")
  means you ran it in the parent: move to the new folder. `name_taken`
  means the name is, or was, some branch's that main can see: pick
  another. `ambiguous_branch` ("names more than one branch … name it by
  its id") means two old branches share a name: pass the id it lists.
- **A plain copy of a branch folder is not that branch.** Writers refuse
  there with `copied_branch` ("… is a copy of branch <name>, which is
  <original>: two folders never record to one branch"). Either start a
  branch of its own in the copy (`lap branch start <name> --from
  <original>`, the way to make a branch of a branch by copying), or do the
  work in the original folder.
- **Say which branch you record to.** Where branches exist, `lap commit`
  and `lap session start` need `--branch <name>` (`--branch main` in the
  first folder), or `LAP_BRANCH=<name>` in the environment. A missing or
  wrong name is refused and the error names the folder's branch: it means
  you may be in the wrong folder — check before retrying. A repository
  with no branches ignores `LAP_BRANCH` (an explicit `--branch` is still
  checked).
- **Merging back**, in the parent folder: commit its own work, `git merge`
  the branch, then `lap merge <branch>` (try `--dry-run` first) — always in
  that order. lap adopts the history `git merge` brought; on
  `git_merge_first` run the `git merge` you skipped, then `lap merge`
  again. Never reach for `--copy-from-folder` unless the user asks for
  it. lap adopts
  every branch commit it can place, with a `from` link to the original; a
  file where the branch's work conflicts with the parent's stops, and what
  is left shows in `lap status` — commit it as usual, citing the branch
  commits (`#<hash>`) it stands for. A change the parent had already made
  the same way is reported as already done, not as a conflict; a file an
  earlier merge stopped stays stopped (the report says so). The branch's
  `lap amend` corrections come along with its commits.
- **Branches of branches.** A branch folder can start branches too
  (`lap branch start sub --from ../proj-parser`), and merge them back the
  same way, in its own folder. A branch of a branch can also go straight
  into main: `git merge` it there, then `lap merge sub`, which adopts the
  work of the branch between up to where `sub` started, too. `lap branch
  list` in main shows `sub` under `parser`.
- **Merging the parent into a branch is not supported.** Never `git merge`
  the parent (main) into a branch folder to stay current: its changes would
  show as the branch's own pending edits and, if committed, come back to the
  parent as branch work. To take in the parent's newer work, merge the
  branch back, then start a fresh branch.
- Ids repeat across folders (both go on from the base). In text that
  leaves the folder — a ticket comment, another branch's commit — cite
  commits by hash and sessions as `<branch>/S<n>`. lap prints a branch's
  sessions that way, and `lap rr`, `log --session` and `search --session`
  take it: `lap rr parser/S4`, from the parent after a merge too.
- **Seeing branches.** `lap branch list` shows each branch started here
  (and those known only by chunks git brought) with its state: active,
  merged, partly merged, missing. `--branch <name>` on `log`, `show`, `rr`
  and `session list` reads that branch's history from here. A branch whose
  folder moved is `missing`: `lap branch move <branch> <path>` points the
  registry at its new place, `lap branch forget <branch>` drops it.

## Using the history (do this before changing unfamiliar code)

- Why does this line exist? →
  `lap search --file <f> --line <n>` (current line number; pending edits
  are reported as pending).
- When did this string appear/disappear? →
  `lap search --text "<str>" [--added|--removed]`.
- What happened in a task? → `lap session list`, then
  `lap log --session S<n>`.
- Read one commit in full (intent, behavior, diff): `lap show L<n>`, or
  by hash or hash prefix (`lap show fa9cebd`, `#fa9cebd` works too); add
  `--full-file` to see the whole file as of that commit.
- **Review a whole piece of work** — before handing it over, or to catch up
  on someone else's: `lap rr <session>` prints the trajectory (commits in
  order, grouped under their shared intent, each with its behavior) and
  the net change (each file diffed from before the
  work to after, so cancelled-out edits vanish and many commits to one
  function read as a single change). `lap rr <from> <to>` reviews a range
  (ids or hashes);
  `lap rr` alone reviews the latest session.

## Sanity

`lap verify` checks the log's hash chain; `lap verify --deep` also replays
history against the shadow store and snapshot cache. `lap rebuild`
reconstructs every cache from the log — run it after copying a repo by its
log alone, deleting a cache under `.lap/`, or when a cache looks wrong.
Truth lives in the log; the caches are regenerable. Three files are not
caches and are never rebuilt: `.lap/lineage` (what makes a folder a
branch — never delete it, or the folder takes itself for main),
`.lap/parent` and `.lap/branches.json` (this machine's record of where a
branch's parent and branches are).
