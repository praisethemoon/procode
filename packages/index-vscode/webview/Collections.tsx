/* index-ui.md §4: `kb:/collections` lists every collection with its document
 * count, byte size and oldest fetch date. Rename and delete live here and
 * nowhere else. A collection row opens the sidebar scoped to it.
 *
 * "AND NOWHERE ELSE" IS THE LOAD-BEARING HALF. Deleting a collection forgets a
 * whole research topic, and an affordance for it on a row in a sidebar is one
 * mis-click from a store that has quietly lost a week of reading. Here it is on
 * a page whose whole subject is collections, beside the count of what would go
 * with it.
 *
 * THE OLDEST FETCH DATE IS DERIVED AND THE DERIVATION IS NAMED. `GET
 * /collections` (index-api.md §7) is "names with counts"; §1.3 makes a
 * collection implicit, so there is no record for a date to live on.
 * `view/facts.ts` folds it out of the document list, which is being read
 * anyway.
 *
 * THE TWO WRITES GO THROUGH THE CLI AND MAY NOT BE THERE YET. §7 has
 * `PATCH /collections/{name}` and `DELETE /collections/{name}`; `kb --help`
 * has neither. The calls are made as the specification describes them and a
 * refusal that says the command is unknown is shown as "this build of kb
 * cannot do it", which is a different sentence from "the store refused".
 */

import { Button, IconButton } from "baukasten-ui/core";
import { useState } from "react";

import { KbCollection, KbDocument } from "kb-js/pure";

import { CollectionRow, collectionRows, formatBytes, formatDate } from "../src/view/facts";
import { Codicon, Resolved, useQuery } from "./parts";
import { StoreRefusal, addFiles, call, confirm, notify, scope } from "./rpc";

/* The document count from a `collection_in_use` refusal, or null for any
 * other failure. */
function inUse(e: unknown): number | null {
    if (!(e instanceof StoreRefusal) || e.error.code !== "collection_in_use") {
        return null;
    }
    const documents = e.error.details?.["documents"];
    return typeof documents === "number" ? documents : null;
}

function Row(props: { row: CollectionRow; onChanged: () => void }): JSX.Element {
    const row = props.row;
    const [renaming, setRenaming] = useState(false);
    const [draft, setDraft] = useState(row.name);

    const problem = (e: unknown, what: string): void => {
        const message = e instanceof Error ? e.message : String(e);
        notify(
            "warning",
            /unknown option|unknown command|expects|usage/i.test(message)
                ? `This build of kb cannot ${what} yet: ${message}`
                : message,
        );
    };

    const rename = (): void => {
        const next = draft.trim();
        setRenaming(false);
        if (next === "" || next === row.name) {
            return;
        }
        call("renameCollection", { from: row.name, to: next })
            .then(() => props.onChanged())
            .catch((e: unknown) => problem(e, "rename a collection"));
    };

    /* The store is asked first without the documents. An empty collection
     * simply goes; one that holds documents is refused with §11's
     * `collection_in_use`, and the count in that refusal — the store's, as of
     * now, not the row's as of the last refresh — is what the reader confirms
     * before the documents are forgotten with it. */
    const remove = (): void => {
        const forget = (withDocuments: boolean): Promise<void> =>
            call("deleteCollection", { name: row.name, withDocuments }).then(() => props.onChanged());
        forget(false)
            .catch((e: unknown) => {
                const held = inUse(e);
                if (held === null) {
                    throw e;
                }
                return confirm(
                    `Forget the collection “${row.name}”?`,
                    `It holds ${held} document${held === 1 ? "" : "s"}, and they are forgotten with it.`,
                    "Forget",
                ).then((confirmed) => (confirmed ? forget(true) : undefined));
            })
            .catch((e: unknown) => problem(e, "delete a collection"));
    };

    return (
        <div className="kb-row kb-collection">
            <div className="kb-row-head">
                {renaming ? (
                    <input
                        className="kb-rename"
                        value={draft}
                        aria-label={`Rename ${row.name}`}
                        autoFocus
                        onChange={(e) => setDraft(e.currentTarget.value)}
                        onBlur={rename}
                        onKeyDown={(e) => {
                            if (e.key === "Enter") {
                                e.preventDefault();
                                rename();
                            }
                            if (e.key === "Escape") {
                                e.preventDefault();
                                setDraft(row.name);
                                setRenaming(false);
                            }
                        }}
                    />
                ) : (
                    <button
                        type="button"
                        className="kb-row-title kb-link"
                        /* §4: "a collection row opens the sidebar scoped to
                         * it." */
                        onClick={() => scope(row.name)}
                        title={`Show ${row.name} in the sidebar`}
                    >
                        {row.name === "" ? "(no collection)" : row.name}
                    </button>
                )}
                <span className="kb-grow" />
                <span className="kb-row-actions">
                    <IconButton
                        icon={<Codicon name="new-file" />}
                        variant="ghost"
                        size="sm"
                        aria-label={`Add files to ${row.name}`}
                        title={`Add files to ${row.name}…`}
                        onClick={() => addFiles(row.name)}
                    />
                    <IconButton
                        icon={<Codicon name="edit" />}
                        variant="ghost"
                        size="sm"
                        aria-label={`Rename ${row.name}`}
                        title="Rename"
                        onClick={() => {
                            setDraft(row.name);
                            setRenaming(true);
                        }}
                    />
                    <IconButton
                        icon={<Codicon name="trash" />}
                        variant="ghost"
                        size="sm"
                        className="kb-danger"
                        aria-label={`Forget ${row.name}`}
                        title="Forget this collection and every document in it"
                        onClick={remove}
                    />
                </span>
            </div>
            <div className="kb-row-meta">
                <span>
                    {row.documents} document{row.documents === 1 ? "" : "s"}
                </span>
                <span>{formatBytes(row.bytes)}</span>
                <span title={row.oldestFetch}>
                    {row.oldestFetch === "" ? "no dates" : `oldest ${formatDate(row.oldestFetch)}`}
                </span>
            </div>
        </div>
    );
}

export function Collections(): JSX.Element {
    const collections = useQuery<KbCollection[]>("collections");
    /* The documents, for the one field §4 asks for that `GET /collections` does
     * not carry. The same limitless read the sidebar makes, so the two see the
     * same store. */
    const documents = useQuery<KbDocument[]>("ls", {});

    const refresh = (): void => {
        collections.refresh();
        documents.refresh();
    };

    return (
        <div className="kb-view kb-doc">
            <div className="kb-scroll">
                <header className="kb-head kb-head-actions">
                    <div>
                        <h1 className="kb-head-title">Collections</h1>
                        <div className="kb-small kb-muted">
                            A collection is a flat named scope and a document belongs to exactly one.
                        </div>
                    </div>
                    <Button size="sm" variant="secondary" onClick={() => addFiles()}>
                        <Codicon name="new-file" /> Add files…
                    </Button>
                </header>
                <Resolved state={collections.state} loading="Counting…">
                    {(rows) => (
                        <Resolved state={documents.state} loading="Counting…">
                            {(docs) => {
                                const list = collectionRows(rows, docs);
                                if (list.length === 0) {
                                    return (
                                        <div className="kb-empty">
                                            Nothing indexed yet. Research lands here when an agent
                                            files what it read.
                                        </div>
                                    );
                                }
                                return (
                                    <div className="kb-list">
                                        {list.map((row) => (
                                            <Row
                                                key={row.name}
                                                row={row}
                                                onChanged={refresh}
                                            />
                                        ))}
                                    </div>
                                );
                            }}
                        </Resolved>
                    )}
                </Resolved>
            </div>
        </div>
    );
}
