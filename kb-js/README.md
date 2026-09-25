# kb-js

A typed, tested wrapper over the `kb` command line (`index-api.md` §10).

It spawns the binary, passes `--json`, and maps exit codes to typed errors.
**It does not reimplement retrieval.** §10 states why, and the asymmetry with
`lap-js` is easy to get wrong: reading a lap log is parsing, so `lap-js` folds
it in-process. Searching here requires embedding the query, which requires the
model — and two inference paths that disagree produce a store that answers
differently depending on which door the caller came through. One
implementation, in C.

```ts
import { Kb, isKbError } from "kb-js";

const kb = new Kb({ cwd: workspaceRoot });

/* index-api.md §1.4 finds the project store by walking up from the working
 * directory, like `.git` — so `cwd` decides which store is read. */
const documents = await kb.ls({ collection: "win32-iocp" });
const { hits } = await kb.search("completion port", { k: 10, store: "all" });

try {
    await kb.get("D-9999");
} catch (e) {
    if (isKbError(e)) {
        e.code; /* the store's own word */
        e.spec; /* one of §11's, or null */
    }
}
```

## The two entry points

| entry | holds | for |
|---|---|---|
| `kb-js` | everything, including the spawn | a Node process |
| `kb-js/pure` | the shapes, the readers, §5's staleness | a browser document |

`kb-js` reaches `node:child_process`, which a webview cannot. `kb-js/pure` is
the half that is a pure function of the store's answers, and it is re-exported
from the same sources — the two entry points are not two implementations.

## Exit codes are the contract

| exit | meaning | what this throws |
|---|---|---|
| `0` | success | nothing; the payload is returned |
| `1` | user or store error | `KbError`, carrying §11's code verbatim |
| `2` | internal failure | `KbCrash` |

A binary that will not start, an answer that cannot be parsed and a command
that does not answer within the timeout are all `KbCrash` as well: §11's table
is the vocabulary a caller can act on, and none of those are in it.

## No shell, ever

Every command line is built by `argv.ts` as an **array**, and `run.ts` hands it
to `spawn` with `shell: false`. There is no string anywhere in this package for
a value to be interpolated into, so a query a reader typed cannot become
syntax — `io_uring; rm -rf ~` is one argument containing a semicolon.
`guards.test.ts` refuses `exec`, `execSync`, `shell: true` and a spawn from a
template literal by name.

## Commands that do not exist yet

`search`, `chunk`, `stale`, `refresh` and the two collection writes are built
to the specification and the CLI does not have them yet. Each is the route from
`index-api.md` spelled in the CLI's own idiom; a caller that reaches one today
gets a `usage` or `unknown_command` refusal. `argv.ts` is the one file to
reconcile when they land.

## Tests

```
npm test
```

`cli.test.ts` drives the real binary when `../kb-cli/bin/kb` is built and skips
when it is not, so this package is installable and testable on its own.
