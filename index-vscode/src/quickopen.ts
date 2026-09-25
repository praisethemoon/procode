/* index-ui.md §5: "a command, `Knowledge: Search`, opens a `QuickPick` over the
 * same search. Picking a result opens the document."
 *
 * IT IS THE SAME SEARCH AND NOT A SECOND ONE. The `QuickPick` calls `kb-js`'s
 * `search` with §4's parameters, exactly as the sidebar does, and renders the
 * same three facts off the same hit. A picker with its own query shape would be
 * a second retrieval path in a package whose whole argument (§10) is that there
 * is one.
 *
 * IT IS ALSO WHAT PAYS FOR NOT REGISTERING A `FileSystemProvider`. `editor.ts`
 * records the trade: without a provider, VSCode's own Quick Open drops entries
 * whose scheme has no file service behind it, so `Cmd-P` cannot reach `kb:/D-241`.
 * §5 asks for this command anyway, and it is strictly better for the job — a
 * reader looking for a passage knows a phrase, not an identifier.
 *
 * EVERY KEYSTROKE IS A SEARCH AND EVERY SEARCH IS A PROCESS, so the in-flight
 * one is superseded rather than awaited: `kb` is spawned per call, results
 * arrive out of order under load, and a picker that rendered whichever answer
 * came back last would show the results for a prefix of what is on screen.
 */

import * as vscode from "vscode";

import { Kb, KbHit, isKbError } from "kb-js";

interface HitItem extends vscode.QuickPickItem {
    readonly hit: KbHit;
}

/* One hit, as the three lines a picker has.
 *
 * THE MATCHED PATHS AND THE TIER ARE IN THE DETAIL LINE. §4 says a result found
 * by both paths is a different kind of result from one found by either, and §2
 * shows that on a row — so the picker shows it too rather than being the one
 * surface where the reader cannot see it. */
function item(hit: KbHit): HitItem {
    const marks = [hit.stale ? "$(warning) stale" : null, ...hit.matched, hit.store]
        .filter((m): m is string => m !== null)
        .join(" · ");
    return {
        hit,
        label: hit.title.length > 0 ? hit.title : hit.document,
        description: hit.heading ?? hit.collection,
        /* The snippet, flattened to one line. A picker gives each item a fixed
         * height and a newline in `detail` is a line the reader cannot see. */
        detail: `${hit.snippet.replace(/\s+/g, " ").trim()}${marks.length > 0 ? `   ${marks}` : ""}`,
        alwaysShow: true,
    };
}

export function quickSearch(
    kb: Kb,
    open: (reference: string, chunk: string | null) => void,
): void {
    const picker = vscode.window.createQuickPick<HitItem>();
    picker.title = "Knowledge";
    picker.placeholder = "Search the knowledge base";
    /* `alwaysShow` on every item and no filtering here: the store ranked these
     * by reciprocal rank fusion (§4), and VSCode's own fuzzy filter over the
     * labels would reorder them by a completely different question. */
    picker.matchOnDescription = false;
    picker.matchOnDetail = false;

    let generation = 0;
    let debounce: ReturnType<typeof setTimeout> | undefined;

    const run = (query: string): void => {
        const mine = ++generation;
        if (query.trim().length === 0) {
            picker.items = [];
            picker.busy = false;
            return;
        }
        picker.busy = true;
        kb.search(query, { k: 20, store: "all" })
            .then((result) => {
                /* Superseded: a later keystroke has already asked a better
                 * question and this answer is about a prefix of it. */
                if (mine !== generation) {
                    return;
                }
                picker.items = result.hits.map(item);
                picker.busy = false;
            })
            .catch((e: unknown) => {
                if (mine !== generation) {
                    return;
                }
                picker.busy = false;
                picker.items = [];
                /* §11's refusal in the picker itself rather than as a
                 * notification behind it: a reader whose store needs a reindex
                 * is looking at this list, and a message somewhere else is a
                 * message they read after giving up. */
                picker.title = isKbError(e)
                    ? `Knowledge — ${e.message} (${e.code})`
                    : `Knowledge — ${e instanceof Error ? e.message : String(e)}`;
            });
    };

    picker.onDidChangeValue((value) => {
        if (debounce !== undefined) {
            clearTimeout(debounce);
        }
        /* §2's "debounced as the reader types" applies here for the same
         * reason: every call is a process, and a spawn per keystroke is a
         * spawn per keystroke. */
        debounce = setTimeout(() => run(value), 160);
    });

    picker.onDidAccept(() => {
        const [chosen] = picker.selectedItems;
        if (chosen !== undefined) {
            /* §3.2: opening a search result scrolls to the matching chunk's
             * heading. The picker knows which chunk matched, so it says. */
            open(chosen.hit.document, chosen.hit.chunk);
        }
        picker.hide();
    });

    picker.onDidHide(() => {
        if (debounce !== undefined) {
            clearTimeout(debounce);
        }
        /* Supersede anything still in flight, so a late answer cannot touch a
         * disposed picker. */
        generation += 1;
        picker.dispose();
    });

    picker.show();
}
