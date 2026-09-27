/* The board on disk: `.coboard/log.jsonl`, append-only, one JSON record a line.
 *
 *   {"op":"put","item":{...}}                    an epic, milestone or ticket, whole
 *   {"op":"comment","ticket":"T-3","author":..,"body":..,"at":..}
 *   {"op":"delete","id":"T-3","at":..}
 *   {"op":"archive","id":"E-2","at":..,"by":..,"reason":..}   by and reason optional
 *   {"op":"unarchive","id":"E-2","at":..}
 *
 * The board is the fold of the log: the last `put` of an id wins, a `delete`
 * removes it, comments attach to their ticket in order.
 *
 * ARCHIVING IS ITS OWN RECORD, NOT A FIELD ON THE ITEM. A reader that predates
 * it skips the records it does not know and sees the board as it was; and a
 * writer that predates it — which puts back the whole item as it read it —
 * cannot drop an archive it never saw. An item is archived with its milestone
 * and epic: archiving an epic writes one record, and unarchiving it brings
 * back everything under it. Archived ids stay taken. Every write re-reads
 * the log under a lock first, so the VS Code extension and any number of
 * agents can write to the same board without clobbering each other or
 * handing out the same id twice. The log is small text and is meant to be
 * committed; there is no cache to rebuild.
 */

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

import {
    Archived,
    ArchivedMode,
    Comment,
    Epic,
    Item,
    Kind,
    Milestone,
    PREFIX,
    PRIORITIES,
    SIZES,
    Ticket,
    idNumber,
    keepArchived,
    kindOf,
    statusesOf,
} from "./model";

export const BOARD_DIR = ".coboard";
export const LOG_FILE = "log.jsonl";

export class BoardError extends Error {
    constructor(
        readonly code: "not_found" | "invalid" | "in_use" | "no_board" | "locked" | "unwritable" | "stale_parent",
        message: string,
    ) {
        super(message);
    }
}

/* The first `.coboard/` at or above `from`, like git finds `.git`. */
export function findBoard(from: string): string | null {
    let dir = path.resolve(from);
    for (;;) {
        const probe = path.join(dir, BOARD_DIR);
        try {
            if (fs.statSync(probe).isDirectory()) {
                return dir;
            }
        } catch {
            // not here
        }
        const up = path.dirname(dir);
        if (up === dir) {
            return null;
        }
        dir = up;
    }
}

/* Where the board is for work done in `from`. One board serves every
 * folder of a project, lap branch folders included (their `.coboard/` is
 * a copy git would later have to merge):
 *
 * 1. `override` — COBOARD_DIR, or the Board view's setting: a folder whose
 *    `.coboard/` is the board, or that `.coboard/` itself;
 * 2. in a lap branch folder, its parent folder, which `lap branch start`
 *    wrote to `.lap/parent` — for a branch of a branch, followed up to
 *    the top (main's folder). Never the branch's own `.coboard/`, a copy
 *    as of its git base: a parent with no board yet gets the first one
 *    (`home`), and a parent folder that is gone is `stale-parent` (with
 *    `stale`, the path recorded), which readers and writers refuse;
 * 3. the first `.coboard/` at or above `from`.
 *
 * `via` says which, so a view can tell the reader whose board it shows. */
export function locateBoard(
    from: string,
    override: string | undefined = process.env["COBOARD_DIR"],
): { root: string | null; via: "override" | "lap-parent" | "found" | "stale-parent"; home?: string; stale?: string } {
    if (override && override.trim()) {
        /* relative to the folder worked in, not to whichever process asks
         * (the editor's cwd and the MCP server's differ); ~ is the home */
        let given = override.trim();
        if (given === "~" || given.startsWith("~/")) {
            given = path.join(os.homedir(), given.slice(1));
        }
        const dir = path.resolve(from, given);
        return { root: path.basename(dir) === BOARD_DIR ? path.dirname(dir) : dir, via: "override" };
    }
    let parent = lapParent(from);
    for (let hops = 0; parent && hops < 32; hops++) {
        const up = lapParent(parent); /* a branch of a branch: go on up */
        if (!up || path.resolve(up) === path.resolve(parent)) {
            break;
        }
        parent = up;
    }
    if (parent && isDir(path.join(parent, BOARD_DIR))) {
        return { root: parent, via: "lap-parent" };
    }
    if (parent && isDir(parent)) {
        return { root: null, via: "lap-parent", home: parent };
    }
    if (parent) {
        return { root: null, via: "stale-parent", stale: parent };
    }
    return { root: findBoard(from), via: "found" };
}

/* Why a lap branch folder whose recorded parent is gone has no board: its
 * own `.coboard/` is a stale copy, never used. */
export function staleParentMessage(stale: string): string {
    return (
        `this folder is a lap branch of ${stale}, which is gone, so its board cannot be found ` +
        "(the .coboard/ here is a copy as of the branch's git base and is not used). " +
        "Point COBOARD_DIR (or the Board Folder setting) at the parent's folder, " +
        "or write the parent's new path into .lap/parent here"
    );
}

/* The parent folder a lap branch folder at or above `from` recorded in
 * `.lap/parent`, or null outside a branch folder. */
export function lapParent(from: string): string | null {
    let dir = path.resolve(from);
    for (;;) {
        if (isDir(path.join(dir, ".lap"))) {
            try {
                const p = fs.readFileSync(path.join(dir, ".lap", "parent"), "utf8").trim();
                return p ? p : null;
            } catch {
                return null;
            }
        }
        const up = path.dirname(dir);
        if (up === dir) {
            return null;
        }
        dir = up;
    }
}

function isDir(p: string): boolean {
    try {
        return fs.statSync(p).isDirectory();
    } catch {
        return false;
    }
}

export interface Fields {
    title?: string;
    description?: string;
    status?: string;
    size?: string | null;
    priority?: string;
    assignee?: string | null;
    labels?: readonly string[];
}

export interface CreateInput extends Fields {
    kind: Kind;
    title: string;
    epic?: string | null;
    milestone?: string | null;
}

export interface Placement {
    epic?: string | null;
    milestone?: string | null;
}

interface State {
    items: Map<string, Item>;
    comments: Map<string, Comment[]>;
    /* The items archived themselves, with their archive record. */
    archived: Map<string, Omit<Archived, "via">>;
    next: Record<Kind, number>;
}

/* The item's own archive, or the one it inherits from its milestone or epic. */
function archivedOf(st: State, item: Item): Archived | undefined {
    const own = st.archived.get(item.id);
    if (own !== undefined) {
        return { ...own, via: null };
    }
    const up = item.kind === "ticket" ? [item.milestone, item.epic] : item.kind === "milestone" ? [item.epic] : [];
    for (const id of up) {
        const a = id ? st.archived.get(id) : undefined;
        if (a !== undefined) {
            return { ...a, via: id! };
        }
    }
    return undefined;
}

function now(): string {
    return new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

export class Board {
    readonly dir: string;
    readonly logPath: string;

    /* `root` is the directory that holds `.coboard/`. Nothing is created until
     * the first write. */
    constructor(readonly root: string) {
        this.dir = path.join(root, BOARD_DIR);
        this.logPath = path.join(this.dir, LOG_FILE);
    }

    exists(): boolean {
        return fs.existsSync(this.logPath) || fs.existsSync(this.dir);
    }

    /* ------------------------------------------------------------ reading */

    private load(): State {
        const st: State = {
            items: new Map(),
            comments: new Map(),
            archived: new Map(),
            next: { epic: 1, milestone: 1, ticket: 1 },
        };
        let text = "";
        try {
            text = fs.readFileSync(this.logPath, "utf8");
        } catch {
            return st;
        }
        for (const line of text.split("\n")) {
            if (!line.trim()) {
                continue;
            }
            let rec: Record<string, unknown>;
            try {
                rec = JSON.parse(line) as Record<string, unknown>;
            } catch {
                continue; // a torn final line from a crash; never acknowledged
            }
            if (rec["op"] === "put") {
                const item = rec["item"] as Item;
                const kind = kindOf(item?.id ?? "");
                if (!kind) {
                    continue;
                }
                st.items.set(item.id, item);
                st.next[kind] = Math.max(st.next[kind], idNumber(item.id) + 1);
            } else if (rec["op"] === "comment") {
                const t = String(rec["ticket"]);
                const list = st.comments.get(t) ?? [];
                list.push({ author: String(rec["author"] ?? ""), body: String(rec["body"] ?? ""), at: String(rec["at"] ?? "") });
                st.comments.set(t, list);
            } else if (rec["op"] === "delete") {
                st.items.delete(String(rec["id"]));
                st.archived.delete(String(rec["id"]));
            } else if (rec["op"] === "archive") {
                const id = String(rec["id"]);
                if (st.items.has(id)) {
                    st.archived.set(id, archiveOf(rec));
                }
            } else if (rec["op"] === "unarchive") {
                st.archived.delete(String(rec["id"]));
            }
            // Any other op is from a newer coboard: skipped, as this one's
            // records are skipped by older ones.
        }
        return st;
    }

    /* The item as a reader sees it: a ticket with its comments, and any item
     * with its archive when it has one. */
    private withComments(st: State, item: Item): Item {
        const { archived: _stale, ...plain } = item as Item & { archived?: Archived };
        const archived = archivedOf(st, plain as Item);
        const full = (plain.kind === "ticket" ? { ...plain, comments: st.comments.get(plain.id) ?? [] } : plain) as Item;
        return archived ? ({ ...full, archived } as Item) : full;
    }

    /* Every item, epics then milestones then tickets, each in id order.
     * Archived items are left out unless asked for. */
    all(options: { archived?: ArchivedMode } = {}): Item[] {
        const st = this.load();
        const order: Record<Kind, number> = { epic: 0, milestone: 1, ticket: 2 };
        return [...st.items.values()]
            .map((i) => this.withComments(st, i))
            .filter((i) => keepArchived(i, options.archived))
            .sort((a, b) => order[a.kind] - order[b.kind] || idNumber(a.id) - idNumber(b.id));
    }

    /* Any item by id, archived or not. */
    get(id: string): Item {
        const st = this.load();
        const item = st.items.get(id.trim().toUpperCase());
        if (!item) {
            throw new BoardError("not_found", `no ${id} on this board`);
        }
        return this.withComments(st, item);
    }

    /* ------------------------------------------------------------ writing */

    /* Runs `fn` against a fresh read of the log while holding
     * `.coboard/lock`, and appends whatever records it returns. */
    private write<T>(fn: (st: State) => { records: object[]; result: T }): T {
        try {
            fs.mkdirSync(this.dir, { recursive: true });
            // The log is meant to be committed; the lock never is.
            const ignore = path.join(this.dir, ".gitignore");
            if (!fs.existsSync(ignore)) {
                fs.writeFileSync(ignore, "lock\n");
            }
        } catch (e) {
            const code = (e as NodeJS.ErrnoException).code;
            throw new BoardError("unwritable", `cannot write the board in ${this.dir} (${code ?? String(e)}): is the folder writable?`);
        }
        const lock = path.join(this.dir, "lock");
        const deadline = Date.now() + 5000;
        for (;;) {
            try {
                fs.writeFileSync(lock, String(process.pid), { flag: "wx" });
                break;
            } catch (e) {
                // Only "another writer holds it" is worth waiting for.
                const code = (e as NodeJS.ErrnoException).code;
                if (code !== "EEXIST") {
                    throw new BoardError("unwritable", `cannot take the board's lock in ${this.dir} (${code ?? String(e)}): is the folder writable?`);
                }
                if (Date.now() > deadline) {
                    throw new BoardError("locked", "the board is locked by another writer");
                }
                // A lock older than 30s belongs to a writer that died holding it.
                try {
                    if (Date.now() - fs.statSync(lock).mtimeMs > 30_000) {
                        fs.rmSync(lock, { force: true });
                        continue;
                    }
                } catch {
                    continue; /* released meanwhile: try again */
                }
                Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
            }
        }
        try {
            const { records, result } = fn(this.load());
            if (records.length > 0) {
                fs.appendFileSync(this.logPath, records.map((r) => JSON.stringify(r) + "\n").join(""));
            }
            return result;
        } finally {
            fs.rmSync(lock, { force: true });
        }
    }

    create(input: CreateInput): Item {
        return this.write((st) => {
            const title = cleanTitle(input.title);
            const at = now();
            const id = `${PREFIX[input.kind]}-${st.next[input.kind]}`;
            const base = {
                id,
                title,
                description: input.description ?? "",
                status: checkStatus(input.kind, input.status ?? statusesOf(input.kind)[0]),
                created: at,
                updated: at,
            };
            let item: Item;
            if (input.kind === "epic") {
                item = { ...base, kind: "epic" };
            } else if (input.kind === "milestone") {
                const epic = notArchived(st, need(st, input.epic, "epic", "a milestone needs an epic"));
                item = { ...base, kind: "milestone", epic: epic.id };
            } else {
                const place = resolvePlacement(st, input.epic ?? null, input.milestone ?? null);
                item = {
                    ...base,
                    kind: "ticket",
                    epic: place.epic,
                    milestone: place.milestone,
                    size: checkSize(input.size ?? null),
                    priority: checkPriority(input.priority ?? "medium"),
                    assignee: cleanOptional(input.assignee),
                    labels: cleanLabels(input.labels ?? []),
                    comments: [],
                };
            }
            return { records: [{ op: "put", item: stored(item) }], result: item };
        });
    }

    update(id: string, fields: Fields): Item {
        return this.write((st) => {
            const item = need(st, id, null, "");
            const next: Record<string, unknown> = { ...item, updated: now() };
            if (fields.title !== undefined) {
                next["title"] = cleanTitle(fields.title);
            }
            if (fields.description !== undefined) {
                next["description"] = fields.description;
            }
            if (fields.status !== undefined) {
                next["status"] = checkStatus(item.kind, fields.status);
            }
            const ticketOnly = ["size", "priority", "assignee", "labels"] as const;
            for (const k of ticketOnly) {
                if (fields[k] !== undefined && item.kind !== "ticket") {
                    throw new BoardError("invalid", `${k} is a ticket field and ${item.id} is a ${item.kind}`);
                }
            }
            if (fields.size !== undefined) {
                next["size"] = checkSize(fields.size);
            }
            if (fields.priority !== undefined) {
                next["priority"] = checkPriority(fields.priority);
            }
            if (fields.assignee !== undefined) {
                next["assignee"] = cleanOptional(fields.assignee);
            }
            if (fields.labels !== undefined) {
                next["labels"] = cleanLabels(fields.labels);
            }
            const updated = this.withComments(st, next as unknown as Item);
            return { records: [{ op: "put", item: stored(updated) }], result: updated };
        });
    }

    /* A ticket to another epic and/or milestone (`milestone: null` takes it
     * out of its milestone), or a milestone — with its tickets — to another
     * epic. */
    move(id: string, to: Placement): Item {
        return this.write((st) => {
            const item = need(st, id, null, "");
            const at = now();
            if (item.kind === "epic") {
                throw new BoardError("invalid", "an epic is not inside anything and cannot move");
            }
            if (item.kind === "milestone") {
                if (to.milestone !== undefined && to.milestone !== null) {
                    throw new BoardError("invalid", "a milestone moves to an epic, not into another milestone");
                }
                const epic = notArchived(st, need(st, to.epic, "epic", "moving a milestone needs the epic to move it to"));
                const moved: Milestone = { ...item, epic: epic.id, updated: at };
                const records: object[] = [{ op: "put", item: stored(moved) }];
                for (const t of st.items.values()) {
                    if (t.kind === "ticket" && t.milestone === item.id && t.epic !== epic.id) {
                        records.push({ op: "put", item: stored({ ...t, epic: epic.id, updated: at }) });
                    }
                }
                return { records, result: moved };
            }
            // A new epic without a milestone leaves the old milestone behind:
            // it belongs to the old epic.
            let milestone = to.milestone;
            if (milestone === undefined) {
                milestone = to.epic && to.epic !== item.epic ? null : item.milestone;
            }
            const place = resolvePlacement(st, to.epic ?? (milestone ? null : item.epic), milestone);
            const moved: Ticket = { ...item, epic: place.epic, milestone: place.milestone, updated: at };
            const withC = this.withComments(st, moved);
            return { records: [{ op: "put", item: stored(withC) }], result: withC };
        });
    }

    comment(ticket: string, body: string, author: string): Ticket {
        return this.write((st) => {
            const t = need(st, ticket, "ticket", "");
            if (!body.trim()) {
                throw new BoardError("invalid", "a comment needs a body");
            }
            const c: Comment = { author: author.trim() || "anonymous", body, at: now() };
            const result = { ...(this.withComments(st, t) as Ticket) };
            result.comments = [...result.comments, c] as Comment[];
            return { records: [{ op: "comment", ticket: t.id, ...c }], result };
        });
    }

    /* Archive an item, and with it everything under it. Reversible, and
     * nothing is written to the children. */
    archive(id: string, note: { by?: string; reason?: string } = {}): Item {
        return this.write((st) => {
            const item = need(st, id, null, "");
            if (st.archived.has(item.id)) {
                throw new BoardError("invalid", `${item.id} is already archived`);
            }
            const record = { op: "archive", id: item.id, at: now(), ...optional("by", note.by), ...optional("reason", note.reason) };
            st.archived.set(item.id, archiveOf(record));
            return { records: [record], result: this.withComments(st, item) };
        });
    }

    /* Unarchive an item archived itself. One archived with its milestone or
     * epic comes back when that does. */
    unarchive(id: string): Item {
        return this.write((st) => {
            const item = need(st, id, null, "");
            if (!st.archived.has(item.id)) {
                const inherited = archivedOf(st, item);
                throw new BoardError(
                    "invalid",
                    inherited ? `${item.id} is archived with ${inherited.via}; unarchive ${inherited.via}` : `${item.id} is not archived`,
                );
            }
            st.archived.delete(item.id);
            return { records: [{ op: "unarchive", id: item.id, at: now() }], result: this.withComments(st, item) };
        });
    }

    /* An epic or milestone that still holds anything is refused rather than
     * taking its children with it; a deleted milestone's tickets would have
     * nowhere obvious to go, so empty it first. */
    remove(id: string): void {
        this.write((st) => {
            const item = need(st, id, null, "");
            const children = [...st.items.values()].filter(
                (c) => (c.kind !== "epic" && "epic" in c && c.epic === item.id) || (c.kind === "ticket" && c.milestone === item.id),
            );
            if (children.length > 0) {
                throw new BoardError(
                    "in_use",
                    `${item.id} still holds ${children.map((c) => c.id).join(", ")}; move or delete them first`,
                );
            }
            return { records: [{ op: "delete", id: item.id, at: now() }], result: undefined };
        });
    }
}

/* ------------------------------------------------------------ validation */

function optional(key: string, v: string | undefined): Record<string, string> {
    const s = (v ?? "").trim();
    return s ? { [key]: s } : {};
}

function archiveOf(rec: Record<string, unknown>): Omit<Archived, "via"> {
    return {
        at: String(rec["at"] ?? ""),
        ...(typeof rec["by"] === "string" ? { by: rec["by"] } : {}),
        ...(typeof rec["reason"] === "string" ? { reason: rec["reason"] } : {}),
    };
}

/* A container something is being put into must not be archived: the new
 * item would be archived the moment it was made. */
function notArchived<T extends Item>(st: State, container: T): T {
    const a = archivedOf(st, container);
    if (a) {
        throw new BoardError("invalid", a.via ? `${container.id} is archived with ${a.via}` : `${container.id} is archived`);
    }
    return container;
}

function need(st: State, id: string | null | undefined, kind: Kind | null, missing: string): Item {
    if (!id || !id.trim()) {
        throw new BoardError("invalid", missing || "an id is required");
    }
    const key = id.trim().toUpperCase();
    const item = st.items.get(key);
    if (!item) {
        throw new BoardError("not_found", `no ${key} on this board`);
    }
    if (kind && item.kind !== kind) {
        throw new BoardError("invalid", `${key} is a ${item.kind}, not a ${kind}`);
    }
    return item;
}

/* A ticket's epic and milestone, agreeing with each other: a milestone
 * implies its epic, and an epic given alongside it must be that one. */
function resolvePlacement(st: State, epic: string | null, milestone: string | null): { epic: string; milestone: string | null } {
    if (milestone) {
        const m = notArchived(st, need(st, milestone, "milestone", "") as Milestone);
        if (epic && epic.trim().toUpperCase() !== m.epic) {
            throw new BoardError("invalid", `${m.id} belongs to ${m.epic}, not ${epic.trim().toUpperCase()}`);
        }
        return { epic: m.epic, milestone: m.id };
    }
    const e = notArchived(st, need(st, epic, "epic", "a ticket needs an epic (or a milestone, which implies one)"));
    return { epic: e.id, milestone: null };
}

function stored(item: Item): Item {
    // Archives live in their own records; a put never carries them.
    const { archived: _archived, ...plain } = item as Item & { archived?: Archived };
    if (plain.kind !== "ticket") {
        return plain as Item;
    }
    // Nor comments.
    const { comments: _comments, ...rest } = plain as Ticket;
    return rest as Ticket;
}

function cleanTitle(title: string | undefined): string {
    const t = (title ?? "").replace(/\s+/g, " ").trim();
    if (!t) {
        throw new BoardError("invalid", "a title is required");
    }
    return t;
}

function cleanOptional(v: string | null | undefined): string | null {
    const t = (v ?? "").trim();
    return t ? t : null;
}

function cleanLabels(labels: readonly string[]): string[] {
    return [...new Set(labels.map((l) => l.trim().toLowerCase()).filter((l) => l.length > 0))];
}

function oneOf(what: string, v: string, allowed: readonly string[]): string {
    const s = v.trim().toLowerCase();
    if (!allowed.includes(s)) {
        throw new BoardError("invalid", `${what} "${v}" is not one of ${allowed.join(", ")}`);
    }
    return s;
}

function checkStatus(kind: Kind, v: string): string {
    return oneOf(`${kind} status`, v, statusesOf(kind));
}

function checkSize(v: string | null): string | null {
    return v === null || v.trim() === "" ? null : oneOf("size", v, SIZES);
}

function checkPriority(v: string): string {
    return oneOf("priority", v, PRIORITIES);
}

export type { Epic, Milestone, Ticket, Item };
