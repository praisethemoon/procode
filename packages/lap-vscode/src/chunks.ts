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
