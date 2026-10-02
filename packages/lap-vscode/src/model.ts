/* Pure data layer: parses lap's log (chunks.ts finds its files) into commits
 * and sessions.
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
    op: string; /* "edit" | "create" | "delete" | "untrack" */
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
    /* adopted by lap merge: the hash of the branch commit it came from */
    from: string | null;
    ts: string;
    /* lap amend: intent, behavior and forced above are the latest text;
     * earlier holds every text it replaced, oldest first, and amendedBy /
     * amendedTs who wrote the latest one and when (null: never amended) */
    earlier: EarlierText[];
    amendedBy: string | null;
    amendedTs: string | null;
}

/* A commit's text that a later lap amend replaced. */
export interface EarlierText {
    intent: string;
    behavior: string;
    user: string | null;
    ts: string;
}

export interface SessionRec {
    id: string;
    recIndex: number; /* index of its session_start line in the log */
    /* adopted by lap merge: the hash of the branch's session_start */
    from: string | null;
    msg: string;
    /* the ticket it was started for (--meta ticket=T-n), or null */
    ticket: string | null;
    ts: string;
    endTs: string | null;
    /* how it ended (lap session end --done/--decided/--left), each part
     * null when not given; null when open or ended without one */
    summary: SessionSummary | null;
    commits: CommitRec[];
}

export interface SessionSummary {
    readonly done: string | null;
    readonly decided: string | null;
    readonly left: string | null;
}

/* What one lap merge adopted of a branch. */
export interface MergeRec {
    branch: string; /* the branch's id */
    name: string;
    head: string; /* the branch's last record adopted */
    adopted: number;
    left: number;
    stopped: { file: string; at: string }[];
    ts: string;
}

export interface LapLog {
    /* log order = chronological, oldest first */
    commits: CommitRec[];
    sessions: SessionRec[];
    noSession: CommitRec[];
    activeSessionId: string | null;
    parseErrors: number;
    records: number; /* all record lines folded so far, any kind */
    merges: MergeRec[];
    /* A branch folder's history: where its own part starts (its branch
     * record's index, the last branch record for a branch of a branch,
     * whose history holds the branches it started from) and its name;
     * null in a main folder's. */
    branchAt: number | null;
    branchName: string | null;
}

/* The SHA-256 of one record line's exact bytes, as lowercase hex. */
export type LineHash = (line: string) => string;

export interface LogReader {
    log: LapLog;
    byId: Map<string, SessionRec>;
    byHash: Map<string, CommitRec>; /* for amend records, which name one */
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
            merges: [],
            branchAt: null,
            branchName: null,
        },
        byId: new Map(),
        byHash: new Map(),
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
            from: typeof rec["from"] === "string" ? (rec["from"] as string) : null,
            ts: String(rec["ts"] ?? ""),
            earlier: [],
            amendedBy: null,
            amendedTs: null,
        };
        log.commits.push(c);
        r.byHash.set(c.hash, c);
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
            recIndex,
            from: typeof rec["from"] === "string" ? (rec["from"] as string) : null,
            msg: String(rec["msg"] ?? ""),
            ticket: ticketOf(rec["meta"]),
            ts: String(rec["ts"] ?? ""),
            endTs: null,
            summary: null,
            commits: [],
        };
        log.sessions.push(s);
        r.byId.set(s.id, s);
        if (s.from === null) {
            /* matches the CLI: an adopted session is history, not open */
            log.activeSessionId = s.id;
        }
    } else if (type === "session_end") {
        const s = r.byId.get(String(rec["id"] ?? ""));
        if (s) {
            s.endTs = String(rec["ts"] ?? "");
            const part = (k: string) => (typeof rec[k] === "string" ? (rec[k] as string) : null);
            const summary = { done: part("done"), decided: part("decided"), left: part("left") };
            s.summary = summary.done ?? summary.decided ?? summary.left ? summary : null;
        }
        if (typeof rec["from"] !== "string") {
            log.activeSessionId = null; /* matches the CLI: any end closes */
        }
    } else if (type === "branch") { /* the last one: a branch of a branch */
        log.branchAt = recIndex;
        log.branchName = String(rec["name"] ?? rec["id"] ?? "");
        log.activeSessionId = null; /* a branch starts with none open */
    } else if (type === "merge") {
        const stopped = Array.isArray(rec["stopped"]) ? (rec["stopped"] as Record<string, unknown>[]) : [];
        log.merges.push({
            branch: String(rec["branch"] ?? ""),
            name: String(rec["name"] ?? ""),
            head: String(rec["head"] ?? ""),
            adopted: Number(rec["adopted"] ?? 0),
            left: Number(rec["left"] ?? 0),
            stopped: stopped.map((s) => ({ file: String(s["file"] ?? ""), at: String(s["at"] ?? "") })),
            ts: String(rec["ts"] ?? ""),
        });
    } else if (type === "amend") {
        /* matches the CLI: the commit named takes the text, the last
         * amendment in log order being the latest; no entry of its own */
        const c = r.byHash.get(String(rec["of"] ?? ""));
        if (c) {
            const amended = c.earlier.length > 0;
            c.earlier.push({
                intent: c.intent,
                behavior: c.behavior,
                user: amended ? c.amendedBy : c.user,
                ts: amended ? (c.amendedTs ?? "") : c.ts,
            });
            c.intent = String(rec["intent"] ?? "");
            c.behavior = String(rec["behavior"] ?? "");
            c.forced = rec["forced"] === true;
            c.amendedBy = typeof rec["user"] === "string" ? (rec["user"] as string) : null;
            c.amendedTs = String(rec["ts"] ?? "");
        }
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

/* The ticket in a session_start's meta: a non-empty string, else null. */
export function ticketOf(meta: unknown): string | null {
    const t = meta && typeof meta === "object" ? (meta as Record<string, unknown>)["ticket"] : undefined;
    return typeof t === "string" && t.trim() !== "" ? t.trim() : null;
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
    if (c.op === "untrack") return "untracked (the file was not changed)";
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
    if (c.earlier.length > 0) {
        const n = c.earlier.length;
        out.push(`amended: ${n} time${n === 1 ? "" : "s"}, last ${c.amendedTs ?? ""}${c.amendedBy ? ` by ${c.amendedBy}` : ""}`);
    }
    out.push("intent:");
    for (const line of c.intent.split("\n")) {
        out.push(`  ${line}`);
    }
    out.push("behavior:");
    for (const line of c.behavior.split("\n")) {
        out.push(`  ${line}`);
    }
    c.earlier.forEach((e, i) => {
        out.push(`earlier text ${i + 1} of ${c.earlier.length}, written ${e.ts}${e.user ? ` by ${e.user}` : ""}:`);
        out.push("  intent:");
        for (const line of e.intent.split("\n")) {
            out.push(`    ${line}`);
        }
        out.push("  behavior:");
        for (const line of e.behavior.split("\n")) {
            out.push(`    ${line}`);
        }
    });
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
    /* an untrack takes the file out of lap's view, as a delete does: a
     * later create starts it afresh */
    if (c.op === "delete" || c.op === "untrack") {
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
