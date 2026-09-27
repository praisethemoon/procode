# lap — chunked history and branches (draft)

**Status: draft, not implemented.** This describes what lap will do, not what
it does. Each part moves into `SPEC.md` when it is built, and this file shrinks
to what is still to come. Where it disagrees with `SPEC.md`, `SPEC.md` describes
the lap you have.

## Why

Agents cannot work on one codebase in parallel. lap has one history, one
active session, and the files on disk are shared by everyone working in the
folder: two agents in one folder see each other's edits as their own. This
draft gives each agent its own folder and its own line of history, and brings
that history back when the code comes back.

It rests on four ideas:

1. **Chunked history.** The log becomes a directory of chunk files ordered by
   the hash chain, so different lines of history write different files and
   git never has to merge a log file.
2. **A branch is a lap repository in another folder** whose history is its
   parent's up to a base hash, then its own.
3. **git merges the code; lap adopts the history.** `lap merge` places the
   branch's commits onto the parent's history as far as they go without
   conflict, file by file. Whatever git's merge did beyond that is committed
   by hand, as merged work already is today.
4. **Nothing about where branches live can break a command.** The parent keeps
   a list of its branch folders as hints, and cleans it up where nothing can
   be lost.

## Decisions taken

With the user, 2026-09-27:

| | Decision |
|---|---|
| Chunk size | Sealed at 4 MB, and when a branch starts from it (§Why a branch start seals its parent's chunk). |
| Existing logs | Split automatically, losslessly: no record and no hash changes. |
| Sealed chunks | `lap verify` fails, naming the chunk, when one changed. |
| After a merge | The branch can keep working; a later merge adopts only what is new. |
| Parent into branch | Not in this version. Merge into the parent, start a fresh branch. |
| Ids on adoption | Commits and sessions get the target's next ids, and a `from` link to the original. |
| The board | Not branched: agents in branch folders write to the parent's board. |
| Views | Show branches. The parent keeps a machine-local registry of branch folders. |
| Registry policy | Hints, never errors. Merged-and-gone is pruned silently; unmerged-and-gone is shown as missing. |

## Terms

- **Line of history** (*lineage*): the chain of records one folder appends to.
  The first folder's is `main`; every branch starts its own.
- **Branch**: a folder with its own lineage, started from a **parent** (another
  lineage) at a **base** — the hash of the parent's last record at the time.
- **Head**: the hash of a lineage's last record.
- **Adopt**: to append a branch's commit to the parent's history, translated
  to the parent's line numbers.

## Chunks

### Layout

```
.lap/
  log/
    main.000001.jsonl      sealed
    main.000002.jsonl      sealed
    main.000003.jsonl      open: main appends here
    7c1e9a02d4b8.000001.jsonl   a branch's lineage (after git merge, or in its folder)
  ...caches as before...
  branches.json            the registry (§Registry): local, not committed
  lineage                  this folder's lineage when it is a branch: local, not committed
```

- A chunk is named `<lineage>.<n>.jsonl`: `main` for the first lineage, a
  12-hex-digit id for a branch (§Branches), and `n` counting that lineage's
  chunks from 1. Two copies of one folder never create the same branch id, so
  chunk files never collide.
- A lineage's **open** chunk is its highest `n`; every lower one is **sealed**
  and is never written again. When an append would take the open chunk past
  4 MB (a lap constant, not a setting), the record starts chunk `n + 1`
  instead. A single record larger than 4 MB is a chunk of its own. Starting
  a branch also seals the parent's open chunk: lap creates the parent's
  chunk `n + 1`, empty, so the next append lands there. An open chunk that
  is still empty is not sealed again: a second branch started before the
  parent appended anything shares the first one's base.
- **Which lineage a folder writes** is `main` unless `.lap/lineage` names a
  branch id. `branch start` writes that file. It is machine-local, like the
  registry: after `git merge` the parent folder holds the branch's chunks
  too, so the chunks alone cannot say which lineage is this folder's.
- **Order is the hash chain.** Within a lineage, chunks follow `n`, and a
  chunk's first record carries the previous chunk's last hash as `prev`. A
  branch's first record carries its base as `prev` (§Branches). No manifest
  lists the chunks, so there is nothing for git to conflict on.
- A folder's history is its lineage's chunks, preceded by its parent's
  history up to the base, recursively down to `main`. Chunks of other
  lineages may sit in `.lap/log/` (a `git merge` brings a branch's chunks into
  the parent's folder); they are not part of this folder's history until
  adopted.
- `.gitignore` commits `.lap/log/` instead of `.lap/log.jsonl`.

### Why a branch start seals its parent's chunk

A branch never writes to its parent's chunks, only to its own lineage's. But
its folder holds copies of them. In a git worktree, git checked out the
parent's chunks as its last git commit had them, usually behind the parent's
lap history, and `branch start` brings them up to the parent's head. If the
parent then kept appending to that same open chunk, git would see both sides
change one file differently, and `git merge` would conflict on it. Sealed at
the branch start, the chunk is identical on both sides from then on (an
identical change merges cleanly), and the parent's new records go to a file
the branch never had.

### What changes elsewhere

- **The index** points at (chunk, offset) instead of an offset into one file.
  It is a cache: its format changes freely.
- **Torn tails** can only occur in a lineage's open chunk; readers drop them
  and writers truncate them, as today.
- **`lap verify`** walks each lineage present. A broken chain inside a sealed
  chunk is reported as *"sealed chunk `main.000002.jsonl` was modified"*, not
  as a bare hash mismatch: a sealed chunk only changes by mistake (a bad
  conflict resolution, a repository-wide replace).
- **Hash lookups** (`show #hash`) search every chunk present, including other
  lineages', so a `from` link can be followed from the parent.

### Converting an existing log

The first writing command that finds `.lap/log.jsonl` and no `.lap/log/`
splits the file at record boundaries into `main.000001.jsonl`, … (4 MB each)
and removes it. Records and hashes are unchanged, so `verify` passes before
and after. Readers read either shape and never convert. git sees one deleted
file and some new ones, once.

## Branches

### Starting one

The folder is made by whoever wants it — `git worktree add`, or a plain copy
of the parent folder. lap does not create folders. Then, in the new folder:

```
lap branch start [name] --from <parent folder>
```

1. **History.** If this folder has no `.lap/`, lap copies the parent's history
   (its chunks up to the parent's head). If it has one (a copied folder, or a
   worktree whose git commit carries `.lap/log/`), its head must be a record
   of the parent's history, else `unrelated_history`; lap then brings it up
   to the parent's head. The base is always the parent's head.
2. **Files.** Every tracked file must equal the committed state at the base
   (as `status` would find it clean). Otherwise `not_clean`, listing the
   files: a branch's first commits must not silently absorb differences it
   never made. The two usual causes are named in the error: the worktree was
   checked out from a git commit older than lap's head (git-commit the
   parent's work first), and files lap tracks but git ignores (a worktree
   does not have them; copy them over).
3. **Sealing.** Under the parent's lock, lap seals the parent's open chunk
   (§Why a branch start seals its parent's chunk) and copies the sealed
   version here.
4. **The branch record** starts the new lineage's first chunk:
   ```jsonc
   {"type":"branch","id":"7c1e9a02d4b8","name":"parser-fix",
    "parent":"main","base":"<parent head>","base_chunk":3,
    "user":"...","ts":"...","prev":"<base>"}
   ```
   `id` is the first 12 hex digits of SHA-256 over base, name, time and a
   random nonce; `name` is optional (defaults to the id) and is what people
   type. `parent` is the parent's lineage. `base_chunk` is the parent's
   chunk the base ends: sealing makes the base the last record of a sealed
   chunk, so a folder's history is whole chunks — the parent's `1 …
   base_chunk`, then its own.
5. **Registration.** lap adds the branch to the parent's registry
   (§Registry), under the same lock. If the parent cannot be written
   (read-only), the branch does not start: sealing is what keeps the later
   `git merge` free of log conflicts.

6. **Lineage.** lap writes the branch id to this folder's `.lap/lineage`.

It prints the id and the name. The parent must be a `main` folder: a branch
of a branch is refused with `nested_branch` in this version.

**The recommended layout** keeps the first folder quiet: agents work in
branch folders, and the first folder only merges. Then a branch always
starts from a folder nobody is editing, and a merge never meets pending
work of its own.

Ids inside a branch continue its parent's counters (`L` and `S` numbers after
the base), so one folder never shows the same id twice. Two folders do: the
parent and a branch both go on from the base, and the board is shared. So
text that leaves a folder — a ticket comment, a commit cited from another
branch — names a commit by its hash, and a session as `<branch>/S<n>`.

### Committing: say where

In a folder that is a branch, or that has branches registered, `lap commit`
and `lap session start` require `--branch <name>` (or `--branch main`), and it
must name this folder's own lineage; otherwise `branch_required` or
`wrong_branch`, which name this folder's branch. It costs a flag and catches
the one mistake that corrupts a merge: an agent working in the folder it
thinks it is not in. A folder with no branches at all keeps today's commands
unchanged. The environment variable `LAP_BRANCH` counts as the flag when
the flag is not given, so an orchestrator sets it once per agent and tools
that call lap (the board, the editor views) need not pass it.

### Merging: adopt, then commit the rest

The order is always: commit your own pending work, `git merge` the branch's
code, then

```
lap merge <branch> [--dry-run]
```

in the parent folder. lap finds the branch's chunks in this folder
(`git merge` brought them) or else in the registered branch folder; if
neither has them, `branch_not_found`. The branch's base must be in this
folder's history, else `unrelated_history`. When the branch folder is
reachable, lap seals the branch's open chunk (under the branch's lock, as a
branch start seals its parent's) and copies the branch's chunks into this
folder's `.lap/log/`, so the originals that `from` links and `#hash`
references point to stay readable after the branch folder is gone, whether
or not git carried them. Sealed first, every copy is the chunk's final
content: a later `git merge` of the branch finds the same file on both
sides.

Then, per file, lap walks the branch's commits to that file in order,
starting after what an earlier merge adopted, and places each on the
parent's version:

- **Three versions.** *Base*: the file as of the branch's base (the parent's
  history replayed to it). *Parent*: the file as the parent has it now (its
  shadow). *Branch*: the base plus the branch's commits so far.
- **Offsets.** The parent's changes since the base are the diff base → parent.
  A branch commit whose region does not overlap or touch any of them moves by
  the net lines the parent added or removed above it: an import added at line
  4 on the parent puts the branch's insertion before line 51 before line 52.
  Each adopted commit shifts the later ones in turn.
- **Conflict.** A region that overlaps a parent change, or touches one (both
  insert at the same point, where either order is plausible), is a conflict.
  **That file stops**: none of the branch's later commits to it are adopted,
  in this merge or later ones. Other files carry on. The diff is lap's own
  (§Edit detection), effort cap included: a file the parent rewrote past
  the cap is one change from top to bottom, and every branch commit to it
  conflicts.

Each adopted commit is appended to the parent's lineage as an ordinary commit
with the parent's next id, the translated coordinates, and

```jsonc
"from":"<the branch commit's hash>"
```

The branch's sessions come across the same way: a new session id, the same
purpose and meta (so `--meta ticket=T-12` still finds the work), and `from`.
A branch starts with no active session (the `branch` record ends whatever
the parent had open, for that lineage), so every branch commit belongs to a
session the branch started, or to none. In detail:

- A branch `session_start` is appended when first met, with `from`; a
  `session_end` the branch wrote is appended too. A session still open at
  the branch's head stays open in the parent until a later merge carries
  its end, and that later merge appends its new commits to the session
  already adopted (found by `from`), not to a new one.
- Adopted records never change the parent's own active session: a
  `session_start` or `session_end` with `from` is history, not the
  parent's state.
- Adopted commits keep their intent, behavior, `forced` and `user` as the
  branch wrote them; the message checks are not run again.

Adoption writes the parent's history and shadow, never the working tree
(core rule 4).

Last, a **merge record** closes the run:

```jsonc
{"type":"merge","branch":"7c1e9a02d4b8","name":"parser-fix",
 "head":"<branch hash adopted up to>","adopted":41,"left":6,
 "stopped":[{"file":"src/foo.c","at":"<first branch commit not adopted>"}],
 "user":"...","ts":"...","prev":"..."}
```

A later merge of the same branch starts after `head`, and keeps every file in
`stopped` stopped.

**What is left.** The working tree holds what `git merge` made; the shadow
holds the parent's history plus what was adopted. The difference — conflict
resolutions, stopped files, anything git did that lap could not place —
shows in `lap status` and is committed as usual, with an intent that cites the
branch commits it stands for (`#9f3e21`). When everything was adopted and git
merged cleanly, the difference is empty.

`--dry-run` reports what would be adopted and where each file would stop,
writing nothing.

### Registry

The parent's `.lap/branches.json` lists the branches started from it:

```jsonc
[{"id":"7c1e9a02d4b8","name":"parser-fix","path":"/abs/path/to/folder",
  "base":"<hash>","started":"<ts>"}]
```

It is **machine-local**: a path means nothing on another machine, so it is
not committed. It is neither history nor a cache — it cannot be rebuilt from
the log — and it is **hints only**:

1. **No command fails because of it.** A missing, moved or reused folder is
   never an error; the only error is `lap merge` finding the branch's history
   nowhere.
2. **An entry is checked, never trusted:** it counts only if its folder exists
   and that folder's `.lap` is that branch (by id).
3. **Merged and gone → dropped silently** by the next writing command, when a
   merge adopted the branch up to its head and the folder is gone: the
   history is in the parent's chunks, nothing is lost.
4. **Unmerged, or stopped at a conflict, and gone → kept**, shown as
   `missing` with its last path. `lap branch forget <branch>` drops it;
   `lap branch move <branch> <path>` points it at a moved folder.

### Commands

- `lap branch start [name] --from <parent>` — above.
- `lap branch list [--json]` — this folder's branches from the registry, each
  with its state: `active`, `merged` (up to head), `partly merged` (stopped
  files), `missing`; commits since base and since the last merge. In a branch
  folder, also its own name, parent and base.
- `lap branch forget <branch>`, `lap branch move <branch> <path>`.
- `lap merge <branch> [--dry-run]`.
- `lap log`, `show`, `rr` accept `--branch <name>` to read another lineage
  present in this folder (a merged-in branch's chunks, or a registered
  folder's), so a branch's work can be reviewed from the parent.
- `lap rr` of an adopted session shows the parent's view, with each commit's
  `from` link.

### The board

Not branched. An agent in a branch folder works the parent's board: coboard
gets a way to be pointed at another folder's `.coboard/` (a setting, and an
environment variable for agents), and uses its existing write lock. One board
means one answer to "is T-93 done?".

### Views

Lap History and the board's review read branches through `lap branch list
--json` and `--branch`: each branch with its state, its sessions and commits,
and after a merge, adopted commits linking to their originals and stopped
files listed. A `missing` branch shows its last path and the two ways to fix
it.

## Core rules that change

- Rule 4 drops "There is no … branch, merge": lap still never writes tracked
  files, and now has branches and merges of history.
- Rule 6 becomes: *one folder, one line of history.* Parallel work happens in
  other folders, as branches; work from elsewhere (a pull request) still lands
  in the working tree and is committed edit by edit.
- "History is linear by construction" holds per lineage.

## Out of scope

- Bringing a parent's newer work into a branch (merging the other way).
- Creating or deleting folders, or running git: lap records, git and the user
  move code.
- Adopting across unrelated histories; merging two branches with each other
  without their common parent.
- A branch of a branch (`nested_branch`). Every branch starts from a `main`
  folder.

## Open questions

1. **`--branch` everywhere, or only in shared folders?** Settled: required
   wherever branches exist, with `LAP_BRANCH` standing in for the flag.
2. **Stopped files and later branch work.** A stopped file stays stopped for
   that branch. Should a later `lap merge --resume <file>` retry it once the
   parent resolved the conflict? Deferred unless it comes up.
3. **Chunk size as a constant.** 4 MB is a lap constant, not a setting, so
   every folder behaves alike. Revisit only with evidence.
