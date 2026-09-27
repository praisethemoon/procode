/* Pure data layer: parses .lap/log.jsonl into commits and sessions.
 * No vscode imports here so it stays unit-testable with plain node.
 * Record schema: see cli/lap-cli/SPEC.md. The log is append-only JSONL; a torn
 * final line (a writer mid-append) is tolerated and counted, never fatal.
 *
 * A commit's hash is the SHA-256 of its record line, never stored in the
 * record. The reader is handed the hash function, so this module needs no
 * node crypto and the webview can share its types.
 *
 * The reader is incremental: the extension feeds only newly appended,
 * newline-terminated text and the model folds it into the same LapLog —
 * a watcher tick costs O(new bytes), never O(log).
 */

export interface CommitRec {
    kind: "commit";
    id: string;
    recIndex: number; /* index of this record line in the log (all kinds) */
    user: string | null;
    session: string | null;
    file: string;
    op: string; /* "edit" | "create" | "delete" */
    oldStart: number;
    oldLines: number;
    newStart: number;
    newLines: number;
    eofNl: boolean;
    oldText: string[];
    newText: string[];
    hash: string; /* SHA-256 of the record line, 64 lowercase hex */
    intent: string; /* why the edit exists; shared by edits for one goal */
    behavior: string; /* what this edit makes the code do */
    forced: boolean; /* recorded with --force-message */
    ts: string;
}

export interface SessionRec {
    id: string;
    msg: string;
    ts: string;
    endTs: string | null;
    commits: CommitRec[];
}

export interface LapLog {
    /* log order = chronological, oldest first */
    commits: CommitRec[];
    sessions: SessionRec[];
    noSession: CommitRec[];
    activeSessionId: string | null;
    parseErrors: number;
    records: number; /* all record lines folded so far, any kind */
}

/* The SHA-256 of one record line's exact bytes, as lowercase hex. */
export type LineHash = (line: string) => string;

export interface LogReader {
    log: LapLog;
    byId: Map<string, SessionRec>;
    hash: LineHash;
}

function asStringArray(v: unknown): string[] {
    if (!Array.isArray(v)) {
        return [];
    }
    return v.filter((x): x is string => typeof x === "string");
}

export function createReader(hash: LineHash): LogReader {
    return {
        log: {
            commits: [],
            sessions: [],
            noSession: [],
            activeSessionId: null,
            parseErrors: 0,
            records: 0,
        },
        byId: new Map(),
        hash,
    };
}

function foldLine(r: LogReader, line: string): void {
    const log = r.log;
    let rec: Record<string, unknown>;
    try {
        rec = JSON.parse(line) as Record<string, unknown>;
    } catch {
        log.parseErrors++;
        return;
    }
    const recIndex = log.records++;
    const type = rec["type"];
    if (type === "commit") {
        const c: CommitRec = {
            kind: "commit",
            id: String(rec["id"] ?? "?"),
            recIndex,
            user:
                typeof rec["user"] === "string"
                    ? (rec["user"] as string)
                    : null,
            session:
                typeof rec["session"] === "string"
                    ? (rec["session"] as string)
                    : null,
            file: String(rec["file"] ?? "?"),
            op: String(rec["op"] ?? "edit"),
            oldStart: Number(rec["old_start"] ?? 0),
            oldLines: Number(rec["old_lines"] ?? 0),
            newStart: Number(rec["new_start"] ?? 0),
            newLines: Number(rec["new_lines"] ?? 0),
            eofNl: Boolean(rec["eof_nl"] ?? true),
            oldText: asStringArray(rec["old_text"]),
            newText: asStringArray(rec["new_text"]),
            hash: r.hash(line),
            intent: String(rec["intent"] ?? ""),
            behavior: String(rec["behavior"] ?? ""),
            forced: rec["forced"] === true,
            ts: String(rec["ts"] ?? ""),
        };
        log.commits.push(c);
        if (c.session !== null) {
            const s = r.byId.get(c.session);
            if (s) {
                s.commits.push(c);
            } else {
                log.noSession.push(c); /* orphaned reference */
            }
        } else {
            log.noSession.push(c);
        }
    } else if (type === "session_start") {
        const s: SessionRec = {
            id: String(rec["id"] ?? "?"),
            msg: String(rec["msg"] ?? ""),
            ts: String(rec["ts"] ?? ""),
            endTs: null,
            commits: [],
        };
        log.sessions.push(s);
        r.byId.set(s.id, s);
        log.activeSessionId = s.id;
    } else if (type === "session_end") {
        const s = r.byId.get(String(rec["id"] ?? ""));
        if (s) {
            s.endTs = String(rec["ts"] ?? "");
        }
        log.activeSessionId = null; /* matches the CLI: any end closes */
    } else {
        /* "init", "snapshot"-style future kinds: counted, not visualized */
    }
}

/* How many of the first `got` bytes an incremental reader may consume: up
 * to and including the last newline, never a byte more. Advancing past a
 * record boundary is what would desync a reader when the CLI repairs a
 * torn tail (truncate + append) — those bytes must stay unread so the
 * replacement record is read whole. */
export function consumableBytes(buf: Uint8Array, got: number): number {
    for (let i = got - 1; i >= 0; i--) {
        if (buf[i] === 0x0a) {
            return i + 1;
        }
    }
    return 0;
}

/* Feeds newline-terminated text (complete lines only — the caller keeps any
 * partial trailing bytes until their newline arrives). */
export function readerFeed(r: LogReader, text: string): void {
    for (const line of text.split("\n")) {
        if (line.trim().length > 0) {
            foldLine(r, line);
        }
    }
}

/* One-shot parse; a torn (unterminated) final line is dropped and counted. */
export function parseLog(text: string, hash: LineHash): LapLog {
    const r = createReader(hash);
    const nl = text.lastIndexOf("\n");
    readerFeed(r, nl < 0 ? "" : text.slice(0, nl + 1));
    if (nl + 1 < text.length && text.slice(nl + 1).trim().length > 0) {
        r.log.parseErrors++;
    }
    return r.log;
}

/* The record's UTC timestamp in the viewer's time zone and locale: the
 * date, a space, and the time to the second, e.g. "09/20/2026 10:51:07 PM".
 * Seconds, because an agent records several commits a minute. */
export function localTime(iso: string): string {
    const d = new Date(iso);
    if (isNaN(d.getTime())) {
        return iso;
    }
    const date = d.toLocaleDateString(undefined, { year: "numeric", month: "2-digit", day: "2-digit" });
    const time = d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    return `${date} ${time}`;
}

/* A hash's short form: its first 7 hex digits. */
export function shortHash(hash: string): string {
    return hash.slice(0, 7);
}

/* First line of a (possibly multiline) message. */
export function summaryLine(msg: string): string {
    const nl = msg.indexOf("\n");
    return nl < 0 ? msg : msg.slice(0, nl);
}

/* Escapes markdown syntax while KEEPING ordinary spaces, so the renderer
 * can word-wrap. vscode's MarkdownString.appendText replaces every space
 * with &nbsp; (to preserve tooltip indentation), which makes long comment
 * lines unwrappable — these helpers exist to avoid it.
 */
export function mdEscape(text: string): string {
    return text.replace(/[\\`*_{}[\]()#+\-.!~<>|]/g, (m) => "\\" + m);
}

/* Multiline prose -> escaped markdown with hard line breaks. */
export function mdProse(text: string): string {
    return text.split("\n").map(mdEscape).join("  \n");
}

/* Human region label, mirroring the CLI's region_describe(). */
export function regionLabel(c: CommitRec): string {
    if (c.newLines === 0 && c.oldLines > 0) {
        return c.oldLines === 1
            ? `line ${c.oldStart} (deleted)`
            : `lines ${c.oldStart}-${c.oldStart + c.oldLines - 1} (deleted)`;
    }
    if (c.oldLines === 0 && c.newLines > 0) {
        return c.newLines === 1
            ? `line ${c.newStart} (insertion)`
            : `lines ${c.newStart}-${c.newStart + c.newLines - 1} (insertion)`;
    }
    if (c.newLines === 1) {
        return `line ${c.newStart}`;
    }
    return `lines ${c.newStart}-${c.newStart + c.newLines - 1}`;
}

/* Text rendering of one commit, in the same shape as `lap show`. */
export function renderCommit(c: CommitRec): string {
    const out: string[] = [];
    out.push(
        `commit ${c.id} ${c.hash}` +
            (c.session ? `  (session ${c.session})` : ""),
    );
    out.push(`date: ${c.ts}`);
    out.push(`file: ${c.file}  (${c.op})`);
    if (c.forced) {
        out.push("forced: the message checks were skipped (--force-message)");
    }
    out.push("intent:");
    for (const line of c.intent.split("\n")) {
        out.push(`  ${line}`);
    }
    out.push("behavior:");
    for (const line of c.behavior.split("\n")) {
        out.push(`  ${line}`);
    }
    out.push("diff:");
    out.push(
        `@@ -${c.oldStart},${c.oldLines} +${c.newStart},${c.newLines} @@`,
    );
    for (const line of c.oldText) {
        out.push(`- ${line}`);
    }
    for (const line of c.newText) {
        out.push(`+ ${line}`);
    }
    out.push("");
    return out.join("\n");
}

/* A file's content at a point in history. */
export interface FileState {
    lines: string[];
    eofNl: boolean;
    exists: boolean;
}

function applyCommit(state: FileState, c: CommitRec): void {
    if (c.op === "delete") {
        state.lines = [];
        state.eofNl = true;
        state.exists = false;
        return;
    }
    state.exists = true;
    const head = state.lines.slice(0, c.oldStart - 1);
    const tail = state.lines.slice(c.oldStart - 1 + c.oldLines);
    state.lines = head.concat(c.newText, tail);
    state.eofNl = c.eofNl;
}

/* Replay, seeded when possible: snapshotsText is the raw content of the
 * file's .lap/snapshots sidecar (or null). Both "at" values and
 * uptoRecIndex are RECORD indices. Seeds from the nearest snapshot at or
 * before the target and applies the commits after it; with no usable
 * snapshot it replays from birth. Identical output either way — snapshots
 * change only the speed (the replay contract in cli/lap-cli/SPEC.md). */
export function replaySeeded(
    log: LapLog,
    file: string,
    uptoRecIndex: number,
    snapshotsText: string | null,
): FileState {
    let seedAt = -1;
    const state: FileState = { lines: [], eofNl: true, exists: false };
    if (snapshotsText) {
        for (const line of snapshotsText.split("\n")) {
            if (line.trim().length === 0) {
                continue;
            }
            try {
                const s = JSON.parse(line) as Record<string, unknown>;
                const at = Number(s["at"] ?? -1);
                if (
                    typeof s["content"] === "string" &&
                    at > seedAt &&
                    at <= uptoRecIndex
                ) {
                    seedAt = at;
                    const content = s["content"] as string;
                    state.lines =
                        content.length === 0 ? [] : content.split("\n");
                    if (
                        state.lines.length > 0 &&
                        state.lines[state.lines.length - 1] === ""
                    ) {
                        state.lines.pop();
                    }
                    state.eofNl = Boolean(s["eof_nl"] ?? true);
                    state.exists = true;
                }
            } catch {
                /* damaged cache line: ignore; correctness is unharmed */
            }
        }
    }
    for (const c of log.commits) {
        if (
            c.file === file &&
            c.recIndex > seedAt &&
            c.recIndex <= uptoRecIndex
        ) {
            applyCommit(state, c);
        }
    }
    return state;
}

/* Serializes a FileState back to file bytes (inverse of line splitting). */
export function stateText(s: FileState): string {
    if (s.lines.length === 0) {
        return "";
    }
    return s.lines.join("\n") + (s.eofNl ? "\n" : "");
}
