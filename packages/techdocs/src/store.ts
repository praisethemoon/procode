/* The pages in a workspace (specs/techdocs.md §1–§3).
 *
 *     .techdocs/
 *       next           the next id's number, so a deleted id is never reused
 *       A-1/index.html the page
 *       A-1/page.json  { id, title, description, keywords, createdAt, updatedAt }
 *
 * CLAIMING AN ID IS A mkdir. Creating A-<n> fails when it exists, so two
 * agents publishing at once each end up with their own directory without a
 * lock, and the counter only ever has to be a lower bound.
 *
 * THE PAGE IS WRITTEN FIRST, THE METADATA SECOND, each through a temporary
 * file and a rename. A reader that finds page.json finds its page; a
 * directory with no page.json is not a page yet and is skipped.
 */

import * as fs from "node:fs";
import * as path from "node:path";

export const STORE_DIR = ".techdocs";
export const PAGE = "index.html";
export const META = "page.json";
const NEXT = "next";

export const MAX_TITLE = 200;
export const MAX_DESCRIPTION = 2000;
export const MAX_PAGE_BYTES = 5 * 1024 * 1024;
export const MAX_KEYWORDS = 10;
export const MAX_KEYWORD = 40;

export type TechdocsErrorCode = "invalid" | "not_found" | "bad_id";

export class TechdocsError extends Error {
    constructor(
        readonly code: TechdocsErrorCode,
        message: string,
    ) {
        super(message);
        this.name = "TechdocsError";
    }
}

export interface Page {
    readonly id: string;
    readonly title: string;
    readonly description: string;
    /* Short lowercase words or phrases; [] for a page published without. */
    readonly keywords: readonly string[];
    readonly createdAt: string;
    readonly updatedAt: string;
    /* The page's size, read from disk, never stored. */
    readonly bytes: number;
}

export interface PublishInput {
    readonly title: string;
    readonly html: string;
    readonly description?: string;
    /* Present: the page's keywords, replacing any it had. Absent: a new
     * page has none, a republished one keeps its own. */
    readonly keywords?: readonly string[];
    /* Present: replace that page. Absent: create the next one. */
    readonly id?: string;
}

const ID = /^A-([1-9][0-9]{0,8})$/;

/* `A-<n>` and nothing else — which is what keeps an id from ever naming a
 * path: no separator, no dot, no leading zero that would let two spellings
 * name one page. */
export function isPageId(id: string): boolean {
    return ID.test(id);
}

function idNumber(id: string): number {
    const m = ID.exec(id);
    return m ? Number(m[1]) : 0;
}

/* The directory holding `.techdocs/`, walking up from `from`; null when
 * none. */
export function findTechdocs(from: string): string | null {
    let dir = path.resolve(from);
    for (;;) {
        try {
            if (fs.statSync(path.join(dir, STORE_DIR), { throwIfNoEntry: false })?.isDirectory()) {
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

/* Where a first page goes when there is no store yet: the
 * enclosing git repository's root, or `from` itself outside one. */
export function defaultRoot(from: string): string {
    let dir = path.resolve(from);
    for (;;) {
        if (fs.existsSync(path.join(dir, ".git"))) {
            return dir;
        }
        const up = path.dirname(dir);
        if (up === dir) {
            return path.resolve(from);
        }
        dir = up;
    }
}

function writeAtomic(file: string, data: string): void {
    const tmp = `${file}.${process.pid}.${Date.now()}.tmp`;
    fs.writeFileSync(tmp, data);
    fs.renameSync(tmp, file);
}

function oneLine(v: unknown, what: string, max: number, required: boolean): string {
    if (v === undefined || v === null) {
        if (required) throw new TechdocsError("invalid", `a page needs a ${what}`);
        return "";
    }
    if (typeof v !== "string") throw new TechdocsError("invalid", `the ${what} must be text`);
    const s = v.trim();
    if (required && s === "") throw new TechdocsError("invalid", `a page needs a ${what}`);
    if (s.length > max) throw new TechdocsError("invalid", `the ${what} is ${s.length} characters; the most is ${max}`);
    return s;
}

/* Keywords as stored: each trimmed, lowercased, inner spaces collapsed;
 * blanks dropped, repeats kept once, in the order given. */
export function normalizeKeywords(v: unknown): string[] {
    if (!Array.isArray(v)) throw new TechdocsError("invalid", "keywords must be a list of words or short phrases");
    const out: string[] = [];
    for (const k of v) {
        if (typeof k !== "string") throw new TechdocsError("invalid", "each keyword must be text");
        const w = k.trim().toLowerCase().replace(/\s+/g, " ");
        if (w === "") continue;
        if (w.length > MAX_KEYWORD) {
            throw new TechdocsError("invalid", `the keyword "${w}" is ${w.length} characters; the most is ${MAX_KEYWORD}`);
        }
        if (!out.includes(w)) out.push(w);
    }
    if (out.length > MAX_KEYWORDS) {
        throw new TechdocsError("invalid", `${out.length} keywords; the most is ${MAX_KEYWORDS}`);
    }
    return out;
}

/* Whether a page carries a keyword, compared as normalizeKeywords stores it. */
export function hasKeyword(a: Page, keyword: string): boolean {
    return a.keywords.includes(keyword.trim().toLowerCase().replace(/\s+/g, " "));
}

export class Techdocs {
    readonly dir: string;

    constructor(readonly root: string) {
        this.dir = path.join(root, STORE_DIR);
    }

    pagePath(id: string): string {
        this.check(id);
        return path.join(this.dir, id, PAGE);
    }

    /* Every page, newest update first; ties by id, newest first. */
    list(): Page[] {
        let names: string[];
        try {
            names = fs.readdirSync(this.dir);
        } catch {
            return [];
        }
        const out: Page[] = [];
        for (const name of names) {
            if (!isPageId(name)) continue;
            const a = this.read(name);
            if (a) out.push(a);
        }
        return out.sort((x, y) =>
            x.updatedAt !== y.updatedAt ? (x.updatedAt < y.updatedAt ? 1 : -1) : idNumber(y.id) - idNumber(x.id),
        );
    }

    get(id: string): { page: Page; html: string } {
        this.check(id);
        const page = this.read(id);
        if (!page) throw new TechdocsError("not_found", `no page ${id}`);
        return { page, html: fs.readFileSync(path.join(this.dir, id, PAGE), "utf8") };
    }

    publish(input: PublishInput, now: Date = new Date()): { page: Page; path: string } {
        const title = oneLine(input.title, "title", MAX_TITLE, true);
        if (/[\r\n]/.test(title)) throw new TechdocsError("invalid", "the title must be one line");
        const description = oneLine(input.description, "description", MAX_DESCRIPTION, false);
        let keywords = input.keywords === undefined ? null : normalizeKeywords(input.keywords);
        if (typeof input.html !== "string" || input.html.trim() === "") {
            throw new TechdocsError("invalid", "a page needs its HTML: html is empty");
        }
        const bytes = Buffer.byteLength(input.html, "utf8");
        if (bytes > MAX_PAGE_BYTES) {
            throw new TechdocsError("invalid", `the page is ${bytes} bytes; the most is ${MAX_PAGE_BYTES}`);
        }
        const stamp = now.toISOString().replace(/\.\d{3}Z$/, "Z");
        let id: string;
        let createdAt = stamp;
        if (input.id !== undefined) {
            this.check(input.id);
            const existing = this.read(input.id);
            if (!existing) throw new TechdocsError("not_found", `no page ${input.id}`);
            id = input.id;
            createdAt = existing.createdAt;
            keywords ??= [...existing.keywords];
        } else {
            id = this.claim();
        }
        const dir = path.join(this.dir, id);
        writeAtomic(path.join(dir, PAGE), input.html);
        writeAtomic(
            path.join(dir, META),
            JSON.stringify({ id, title, description, keywords: keywords ?? [], createdAt, updatedAt: stamp }, null, 2) + "\n",
        );
        const page: Page = { id, title, description, keywords: keywords ?? [], createdAt, updatedAt: stamp, bytes };
        return { page, path: path.join(dir, PAGE) };
    }

    private check(id: string): void {
        if (typeof id !== "string" || !isPageId(id)) {
            throw new TechdocsError("bad_id", `"${String(id)}" is not a page id (A-<n>)`);
        }
    }

    private read(id: string): Page | null {
        try {
            const folder = path.join(this.dir, id);
            const meta = JSON.parse(fs.readFileSync(path.join(folder, META), "utf8")) as Record<string, unknown>;
            const bytes = fs.statSync(path.join(folder, PAGE)).size;
            if (meta["id"] !== id || typeof meta["title"] !== "string") return null;
            const s = (k: string) => (typeof meta[k] === "string" ? (meta[k] as string) : "");
            return {
                id,
                title: s("title"),
                description: s("description"),
                keywords: Array.isArray(meta["keywords"])
                    ? (meta["keywords"] as unknown[]).filter((k): k is string => typeof k === "string")
                    : [],
                createdAt: s("createdAt"),
                updatedAt: s("updatedAt") || s("createdAt"),
                bytes,
            };
        } catch {
            return null;
        }
    }

    /* The next id: past the counter and past every directory there is, and
     * taken by creating its directory, which only one process can do. */
    private claim(): string {
        fs.mkdirSync(this.dir, { recursive: true });
        let n = 1;
        try {
            n = Math.max(n, Number(fs.readFileSync(path.join(this.dir, NEXT), "utf8").trim()) || 1);
        } catch {
            // no counter yet
        }
        for (const name of fs.readdirSync(this.dir)) {
            n = Math.max(n, idNumber(name) + 1);
        }
        for (;;) {
            const id = `A-${n}`;
            try {
                fs.mkdirSync(path.join(this.dir, id));
            } catch (e) {
                if ((e as NodeJS.ErrnoException).code === "EEXIST") {
                    n++;
                    continue;
                }
                throw e;
            }
            writeAtomic(path.join(this.dir, NEXT), `${n + 1}\n`);
            return id;
        }
    }
}
