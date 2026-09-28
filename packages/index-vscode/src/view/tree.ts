/* index-ui.md §2: a collection filed from a folder, drawn as that folder.
 * Pure, so the webview and the tests run the same code.
 *
 * AS VS CODE'S EXPLORER DRAWS IT. Folders first, then files, each in natural
 * order ignoring case (`a2` before `a10`). A chain of folders that each hold
 * only one folder is one row (`tests/fixtures/syntax`), and the folder every
 * path shares is not a row at all: it is `root`, said once above the tree.
 *
 * DOCUMENTS WITHOUT A PATH are not in the tree. Web pages and papers have no
 * folder; they are `loose`, in the order given, drawn as a flat list after it. */

import { KbDocument } from "kb-js/pure";

export interface FileNode {
    readonly name: string;
    readonly path: string;
    readonly doc: KbDocument;
}

export interface FolderNode {
    /* What the row says: one folder, or a compacted chain `a/b/c`. */
    readonly name: string;
    /* Where it is, from the collection's top, without the shared root. */
    readonly path: string;
    readonly folders: readonly FolderNode[];
    readonly files: readonly FileNode[];
    /* Every file under it, at any depth. */
    readonly count: number;
}

export interface CollectionTree {
    /* The folder every path shares, `a/b/`, or "" when they share none. */
    readonly root: string;
    /* The top of the tree: its folders and files are the first rows. */
    readonly top: FolderNode;
    readonly loose: readonly KbDocument[];
}

interface Building {
    readonly folders: Map<string, Building>;
    readonly files: FileNode[];
}

function segments(p: string): string[] {
    return p.replace(/\\/g, "/").split("/").filter((s) => s !== "" && s !== ".");
}

const order = (a: string, b: string): number => a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" }) || (a < b ? -1 : a > b ? 1 : 0);

function finish(b: Building, name: string, at: string): FolderNode {
    /* Compact: while this folder holds one folder and no file, it is that folder. */
    while (b.files.length === 0 && b.folders.size === 1) {
        const [child, inner] = [...b.folders][0];
        name = name === "" ? child : `${name}/${child}`;
        at = at === "" ? child : `${at}/${child}`;
        b = inner;
    }
    const folders = [...b.folders]
        .map(([n, inner]) => finish(inner, n, at === "" ? n : `${at}/${n}`))
        .sort((x, y) => order(x.name, y.name));
    const files = [...b.files].sort((x, y) => order(x.name, y.name));
    const count = files.length + folders.reduce((n, f) => n + f.count, 0);
    return { name, path: at, folders, files, count };
}

export function buildTree(docs: readonly KbDocument[]): CollectionTree {
    const top: Building = { folders: new Map(), files: [] };
    const loose: KbDocument[] = [];
    for (const doc of docs) {
        const parts = segments(doc.path);
        if (parts.length === 0) {
            loose.push(doc);
            continue;
        }
        let at = top;
        for (const s of parts.slice(0, -1)) {
            let next = at.folders.get(s);
            if (!next) {
                next = { folders: new Map(), files: [] };
                at.folders.set(s, next);
            }
            at = next;
        }
        at.files.push({ name: parts[parts.length - 1], path: parts.join("/"), doc });
    }
    /* The shared root: the compacting `finish` does, taken off the top. */
    const shared = finish(top, "", "");
    const root = shared.path === "" ? "" : `${shared.path}/`;
    const strip = (f: FolderNode): FolderNode => ({
        ...f,
        path: f.path.slice(shared.path === "" ? 0 : shared.path.length + 1),
        folders: f.folders.map(strip),
    });
    return { root, top: { ...strip(shared), name: "" }, loose };
}

/* Whether a folder row is open: what the reader chose while the sidebar has
 * been open, else open at the top level and closed below it. */
export function folderOpen(chosen: ReadonlyMap<string, boolean>, path: string, depth: number): boolean {
    return chosen.get(path) ?? depth === 0;
}

/* Whether a collection is drawn as a tree: some document in it has a path. */
export function hasPaths(docs: readonly KbDocument[]): boolean {
    return docs.some((d) => segments(d.path).length > 0);
}

/* What an open collection draws. A search in it stays a flat list of results
 * (each with its path), never a tree. Otherwise the newest page decides: with
 * a path in it, the whole collection is listed and drawn as a tree; without,
 * the collection is drawn flat, a page at a time. */
export function showsTree(searching: boolean, newest: readonly KbDocument[]): boolean {
    return !searching && hasPaths(newest);
}
