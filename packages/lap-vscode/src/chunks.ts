/* Where a folder's history lives on disk. lap keeps it as chunk files in
 * .lap/log/, named <lineage>.<n>.jsonl and read in n order as one stream
 * (cli/lap-cli/SPEC.md §Chunks); a folder lap has not written to since
 * chunks came is still one .lap/log.jsonl. Only the last chunk grows, so a
 * byte offset into the stream, once read up to, never moves. */

import * as fs from "fs";
import * as path from "path";

export interface Chunk {
    path: string;
    size: number;
}

export interface ChunkName {
    lineage: string;
    n: number;
}

const CHUNK_NAME = /^(main|[0-9a-f]{12})\.(\d{6})\.jsonl$/;

/* "main.000003.jsonl" -> { lineage: "main", n: 3 }; null for anything else. */
export function parseChunkName(name: string): ChunkName | null {
    const m = CHUNK_NAME.exec(name);
    if (!m) return null;
    const n = Number(m[2]);
    return n >= 1 ? { lineage: m[1], n } : null;
}

/* The names of one lineage's chunks, in order. Stops at the first missing
 * number: what follows a gap is not part of the chain. */
export function lineageChunks(names: string[], lineage: string): string[] {
    const byN = new Map<number, string>();
    for (const name of names) {
        const c = parseChunkName(name);
        if (c && c.lineage === lineage) byN.set(c.n, name);
    }
    const out: string[] = [];
    for (let n = 1; byN.has(n); n++) out.push(byN.get(n)!);
    return out;
}

/* The first chunk missing from a lineage's numbers while later ones are
 * there: the history is broken at it (lap refuses to read on). null when
 * the numbers run on, or the lineage has no chunk. */
export function missingChunk(names: string[], lineage: string): string | null {
    const have = new Set<number>();
    for (const name of names) {
        const c = parseChunkName(name);
        if (c && c.lineage === lineage) have.add(c.n);
    }
    const max = Math.max(0, ...have);
    for (let n = 1; n <= max; n++) {
        if (!have.has(n)) return `${lineage}.${String(n).padStart(6, "0")}.jsonl`;
    }
    return null;
}

/* Why the folder's history cannot be shown, in lap's words, or null: a
 * chunk missing from the middle of its lineage (or of main's, which a
 * branch folder's history starts with). */
export function historyProblem(lapDir: string): string | null {
    const dir = path.join(lapDir, "log");
    let names: string[] = [];
    try {
        names = fs.readdirSync(dir);
    } catch {
        return null; /* no chunks: nothing to be broken */
    }
    let lineage = "main";
    try {
        lineage = fs.readFileSync(path.join(lapDir, "lineage"), "utf8").trim() || "main";
    } catch {
        /* a main folder */
    }
    for (const l of lineage === "main" ? ["main"] : [lineage, "main"]) {
        const gone = missingChunk(names, l);
        if (gone) return `history chunk ${gone} is missing from ${dir}`;
    }
    return null;
}

/* True when lapDir holds a history in either shape. */
export function hasHistory(lapDir: string): boolean {
    return historyFiles(lapDir).length > 0;
}

/* The files of the folder's history, in order, with their sizes now: the
 * main chunks, or the single-file log when there are none. */
export function historyFiles(lapDir: string): Chunk[] {
    const dir = path.join(lapDir, "log");
    let names: string[] = [];
    try {
        names = fs.readdirSync(dir);
    } catch {
        /* no chunk directory */
    }
    let paths = lineageChunks(names, "main").map((n) => path.join(dir, n));
    if (paths.length === 0) {
        const legacy = path.join(lapDir, "log.jsonl");
        paths = fs.existsSync(legacy) ? [legacy] : [];
    }
    const out: Chunk[] = [];
    for (const p of paths) {
        try {
            out.push({ path: p, size: fs.statSync(p).size });
        } catch {
            break; /* removed while listing: read the rest next time */
        }
    }
    return out;
}

/* The files of branch `lineage`'s history as lapDir holds them: its
 * parent's chunks up to the base chunk its branch record names, then its
 * own. Empty when its first chunk is not here or does not open with its
 * branch record. */
/* The chunk names of lineage's history as the folder holds them, cut after
 * its own chunk `upto` (all of it for 0): main's chunks, or for a branch
 * its parent's history to its base chunk, then its own. A branch of a
 * branch is followed one branch record at a time up to main, as lap does;
 * null when a chunk is missing or the records loop. */
function viewNames(dir: string, names: string[], lineage: string, upto: number, depth: number): string[] | null {
    const own = lineageChunks(names, lineage);
    if (upto > 0 && own.length < upto) return null;
    const mine = upto > 0 ? own.slice(0, upto) : own;
    if (lineage === "main") return mine;
    if (own.length === 0 || depth >= 32) return null;
    let base = 0;
    let parent = "main";
    try {
        const first = fs.readFileSync(path.join(dir, own[0]), "utf8").split("\n", 1)[0];
        const rec = JSON.parse(first) as Record<string, unknown>;
        if (rec["type"] !== "branch" || rec["id"] !== lineage) return null;
        base = Number(rec["base_chunk"] ?? 0);
        parent = String(rec["parent"] ?? "main");
    } catch {
        return null;
    }
    const theirs = viewNames(dir, names, parent, base, depth + 1);
    return theirs === null ? null : [...theirs, ...mine];
}

export function lineageFiles(lapDir: string, lineage: string): Chunk[] {
    const dir = path.join(lapDir, "log");
    let names: string[] = [];
    try {
        names = fs.readdirSync(dir);
    } catch {
        return [];
    }
    const view = viewNames(dir, names, lineage, 0, 0);
    if (view === null || view.length === 0) return [];
    const out: Chunk[] = [];
    for (const n of view) {
        try {
            out.push({ path: path.join(dir, n), size: fs.statSync(path.join(dir, n)).size });
        } catch {
            break;
        }
    }
    return out;
}

/* The files of this folder's own history: a branch folder's lineage (named
 * in .lap/lineage), else main's. */
export function folderFiles(lapDir: string): Chunk[] {
    let lineage = "";
    try {
        lineage = fs.readFileSync(path.join(lapDir, "lineage"), "utf8").trim();
    } catch {
        /* a main folder */
    }
    return lineage ? lineageFiles(lapDir, lineage) : historyFiles(lapDir);
}

/* Reads [from, to) of the history's stream. A chunk that turns out shorter
 * than listed ends the read early; the caller sees how much it got. */
export function readStream(files: Chunk[], from: number, to: number): Buffer {
    const buf = Buffer.alloc(Math.max(0, to - from));
    let got = 0;
    let start = 0;
    for (const f of files) {
        const end = start + f.size;
        const at = from + got;
        if (at < end && got < buf.length) {
            const want = Math.min(end - at, buf.length - got);
            const fd = fs.openSync(f.path, "r");
            try {
                let n = 0;
                while (n < want) {
                    const r = fs.readSync(fd, buf, got + n, want - n, at - start + n);
                    if (r <= 0) break;
                    n += r;
                }
                got += n;
                if (n < want) break;
            } finally {
                fs.closeSync(fd);
            }
        }
        start = end;
    }
    return buf.subarray(0, got);
}
