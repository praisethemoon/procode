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

Built: `SPEC.md` §Chunks and §Branches (lineages, the branch record, the
lineage file, sealing at a branch start, `verify` and `show #hash` over
every lineage present).

## Branches

### Starting one

Built: `SPEC.md` §Branches → Starting one.

### Committing: say where

Built: `SPEC.md` §Branches → Committing: say where.

### Merging

Built: `SPEC.md` §Branches → Merging.

### Registry and commands

Built: `SPEC.md` §Branches → Registry, and the `lap branch` commands.

### The board

Built. Not branched: an agent in a branch folder works the parent's board.
coboard finds it (`locateBoard`, packages/coboard) through `COBOARD_DIR` or
the Board Folder setting first, then the parent path `lap branch start`
wrote to `.lap/parent` (`SPEC.md`, layout), then the board above; writes
keep its lock. One board means one answer to "is T-93 done?".

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
