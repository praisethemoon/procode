/* What the History view shows: the log narrowed by a filter, one page of it.
 * Pure — the extension host runs it and sends only the page to the webview,
 * because the log carries the text of every edit and a long one is megabytes.
 * No vscode import, so the tests run it with plain node.
 *
 * WHAT THE TEXT MATCHES. A commit's id, hash, intent, behavior and file; a
 * session's id and purpose. An id asks for that item alone; a hash prefix of
 * 7 or more hex digits matches the commits whose hash starts with it, and
 * written with `#` it asks for those commits alone.
 *
 * WHAT A MATCH SHOWS. A session found by its own id or message shows every
 * commit it has (that pass the other filters); a session found through its
 * commits shows only those commits. The time range, the kinds of change and
 * the people narrow commits, and a session is shown when any of its commits
 * survive — or, with none at all, when it started in range.
 *
 * ORDER. Newest first, always: the most recent session on top, and its most
 * recent commit first. Commits made outside any session are one group, last.
 */

import { CommitRec, EarlierText, LapLog, SessionRec, regionLabel, summaryLine } from "./model";

export const RANGES = ["recent", "today", "3d", "week", "30d", "all"] as const;
export type Range = (typeof RANGES)[number];

export const RANGE_LABELS: Readonly<Record<Range, string>> = {
    recent: "Most recent",
    today: "Today",
    "3d": "< 3 days",
    week: "< 7 days",
    "30d": "< 30 days",
    all: "All",
};

export const OPS = ["edit", "create", "delete"] as const;
export const STATES = ["active", "open", "ended", "none"] as const;
export type SessionState = (typeof STATES)[number];

export const STATE_LABELS: Readonly<Record<SessionState, string>> = {
    active: "active",
    open: "open",
    ended: "ended",
    none: "no session",
};

export interface HistoryFilter {
    readonly text: string;
    readonly range: Range;
    /* Each: the values any one of which a commit (or session) must have;
     * empty means the field does not filter. */
    readonly ops: readonly string[];
    readonly users: readonly string[];
    readonly states: readonly SessionState[];
}

export const EMPTY_FILTER: HistoryFilter = { text: "", range: "recent", ops: [], users: [], states: [] };

/* The deeper filters, counted for the chevron's badge. */
export function fieldCount(f: HistoryFilter): number {
    return f.ops.length + f.users.length + f.states.length;
}

/* What a key does to a tree row that opens and closes: Enter toggles it,
 * ArrowRight opens it and ArrowLeft closes it; the row's new state, or null
 * when the key leaves it alone (Tab, ArrowDown and the rest move through
 * the tree, never folding what they pass). */
export function treeRowKey(key: string, open: boolean): boolean | null {
    if (key === "Enter") return !open;
    if (key === "ArrowRight") return true;
    if (key === "ArrowLeft") return false;
    return null;
}

export function isFiltering(f: HistoryFilter): boolean {
    return f.text.trim() !== "" || f.range !== "recent" || fieldCount(f) > 0;
}

export const PAGE_SIZE = { grouped: 25, raw: 50 } as const;

function midnight(d: Date): number {
    return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
}

/* The earliest moment in range, in milliseconds, or null for no limit.
 * "Most recent" is the last day anything happened — `latest`, the log's
 * newest moment — from its local midnight: after a weekend, Today is empty
 * and this is not. "Today" starts at local midnight, the way a person means
 * it; the "< n days" ranges count back from now; "All" has no limit. */
export function rangeStart(range: Range, now: Date, latest: number | null = null): number | null {
    const day = 24 * 60 * 60 * 1000;
    switch (range) {
        case "all":
            return null;
        case "recent":
            return latest === null ? null : midnight(new Date(latest));
        case "today":
            return midnight(now);
        case "3d":
            return now.getTime() - 3 * day;
        case "week":
            return now.getTime() - 7 * day;
        default:
            return now.getTime() - 30 * day;
    }
}

/* One commit as a row: what the list shows and what its tooltip says, and
 * none of the edit's text. */
export interface CommitRow {
    readonly id: string;
    readonly ts: string;
    readonly file: string;
    readonly op: string;
    readonly region: string;
    /* The intent's first line. */
    readonly summary: string;
    readonly hash: string;
    readonly intent: string;
    readonly behavior: string;
    readonly forced: boolean;
    /* lap amend: the texts intent and behavior replaced, oldest first */
    readonly earlier: readonly EarlierText[];
    /* adopted by lap merge: the original's hash */
    readonly from: string | null;
    readonly session: string | null;
    readonly user: string | null;
}

export interface SessionRow {
    /* The session's id, or null for the commits made outside any session. */
    readonly id: string | null;
    readonly summary: string;
    readonly msg: string;
    /* The ticket it was started for, or null. */
    readonly ticket: string | null;
    /* The branch it was recorded in, for a branch folder's own session;
     * null for this folder's (or its parent's) sessions. */
    readonly branch: string | null;
    readonly ts: string;
    readonly endTs: string | null;
    readonly state: SessionState;
    /* The commits shown, newest first, and how many the session has in all. */
    readonly commits: readonly CommitRow[];
    readonly total: number;
    /* Found by its own id or message, rather than through its commits. */
    readonly matchedSelf: boolean;
}

export interface HistoryPage {
    readonly grouped: boolean;
    /* Grouped: the sessions on this page. Raw: the commits on it. */
    readonly sessions: readonly SessionRow[];
    readonly commits: readonly CommitRow[];
    /* 0-based, clamped to what exists. */
    readonly page: number;
    readonly pages: number;
    readonly pageSize: number;
    /* Sessions (grouped) or commits (raw) that matched, over every page. */
    readonly total: number;
    /* Everyone who has committed, for the filter's chips. */
    readonly users: readonly string[];
}

const LAP_ID = /^[ls]\d+$/i;
const HASH = /^#?[0-9a-f]{7,64}$/;

interface Text {
    readonly q: string;
    /* An id, or a hash written with `#`: that and nothing else. */
    readonly exact: boolean;
    /* The hex digits when the text could be a hash prefix. */
    readonly hash: string | null;
}

function textOf(q: string): Text {
    const t = q.trim().toLowerCase();
    const hash = HASH.test(t) ? t.replace(/^#/, "") : null;
    return { q: t, exact: LAP_ID.test(t) || (hash !== null && t.startsWith("#")), hash };
}

/* An id asks for that item and nothing else: L12 is not L120. */
function idMatches(id: string, t: Text): boolean {
    return t.exact ? id.toLowerCase() === t.q : id.toLowerCase().includes(t.q);
}

function hashMatches(c: CommitRec, t: Text): boolean {
    return t.hash !== null && c.hash.startsWith(t.hash);
}

function commitText(c: CommitRec, t: Text): boolean {
    if (t.q === "") return true;
    if (t.exact) return idMatches(c.id, t) || hashMatches(c, t);
    return (
        idMatches(c.id, t) ||
        hashMatches(c, t) ||
        c.intent.toLowerCase().includes(t.q) ||
        c.behavior.toLowerCase().includes(t.q) ||
        c.file.toLowerCase().includes(t.q)
    );
}

function sessionText(s: SessionRec, t: Text): boolean {
    if (t.q === "") return false;
    if (t.exact) return idMatches(s.id, t);
    return idMatches(s.id, t) || s.msg.toLowerCase().includes(t.q) || (s.ticket?.toLowerCase().includes(t.q) ?? false);
}

function time(ts: string): number {
    const n = Date.parse(ts);
    return Number.isNaN(n) ? 0 : n;
}

export function stateOf(s: SessionRec, activeId: string | null): SessionState {
    if (s.id === activeId) return "active";
    return s.endTs ? "ended" : "open";
}

export function row(c: CommitRec): CommitRow {
    return {
        id: c.id,
        ts: c.ts,
        file: c.file,
        op: c.op,
        region: regionLabel(c),
        summary: summaryLine(c.intent),
        hash: c.hash,
        intent: c.intent,
        behavior: c.behavior,
        forced: c.forced,
        earlier: c.earlier,
        from: c.from,
        session: c.session,
        user: c.user,
    };
}

/* A session row as the view lays it out: what leads (its ticket, else its
 * session id), the title, what sits at the far end, and the tooltip. A
 * branch folder's own session is named `<branch>/S<n>`. The title drops a
 * leading `T-<n>: ` that only repeats the ticket already leading the row. */
export interface SessionLineParts {
    readonly lead: string | null;
    readonly leadIsTicket: boolean;
    readonly title: string;
    readonly end: string;
    readonly tooltip: string;
}

export function sessionLine(s: SessionRow, filtering: boolean): SessionLineParts {
    const id = s.id === null ? null : s.branch ? `${s.branch}/${s.id}` : s.id;
    const shown = s.commits.length;
    const count = filtering && shown < s.total ? `${shown} of ${s.total} commits` : `${s.total} commit${s.total === 1 ? "" : "s"}`;
    let title = s.summary;
    if (s.ticket) {
        const repeat = new RegExp(`^${s.ticket.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}\\s*:\\s*`, "i");
        const rest = title.replace(repeat, "");
        if (rest.trim() !== "") title = rest;
    }
    const end = [s.ticket ? id : null, count, s.state === "active" ? "active" : null].filter((x) => x !== null).join(" · ");
    const times = `started ${s.ts}${s.endTs ? `, ended ${s.endTs}` : s.state === "active" ? " · active" : ""}`;
    return {
        lead: s.ticket ?? id,
        leadIsTicket: s.ticket !== null,
        title,
        end,
        tooltip: `${id ?? "no session"} · ${s.msg}\n\n${times}`,
    };
}

/* The log's newest moment: its last commit or session start. */
export function latestOf(log: LapLog): number | null {
    let latest: number | null = null;
    for (const ts of [...log.commits.map((c) => c.ts), ...log.sessions.map((s) => s.ts)]) {
        const n = Date.parse(ts);
        if (!Number.isNaN(n) && (latest === null || n > latest)) latest = n;
    }
    return latest;
}

interface Matches {
    readonly sessions: SessionRow[];
    readonly commits: CommitRow[];
    readonly users: readonly string[];
}

export function query(
    log: LapLog,
    filter: HistoryFilter,
    options: { grouped: boolean; page: number; now: Date },
): HistoryPage {
    const m = matching(log, filter, options);
    return options.grouped
        ? paged(true, m.sessions, [], options.page, PAGE_SIZE.grouped, m.users)
        : paged(false, [], m.commits, options.page, PAGE_SIZE.raw, m.users);
}

/* The page a commit is shown on under this filter, or null when the filter
 * hides it. */
export function pageOf(log: LapLog, filter: HistoryFilter, options: { grouped: boolean; now: Date }, id: string): number | null {
    const m = matching(log, filter, options);
    const i = options.grouped
        ? m.sessions.findIndex((s) => s.commits.some((c) => c.id === id))
        : m.commits.findIndex((c) => c.id === id);
    return i < 0 ? null : Math.floor(i / (options.grouped ? PAGE_SIZE.grouped : PAGE_SIZE.raw));
}

function matching(log: LapLog, filter: HistoryFilter, options: { grouped: boolean; now: Date }): Matches {
    const since = rangeStart(filter.range, options.now, latestOf(log));
    const t = textOf(filter.text);
    const inRange = (ts: string) => since === null || time(ts) >= since;
    /* Everything but the text: what a commit must pass wherever it is shown. */
    const passes = (c: CommitRec) =>
        inRange(c.ts) &&
        (filter.ops.length === 0 || filter.ops.includes(c.op)) &&
        (filter.users.length === 0 || (c.user !== null && filter.users.includes(c.user)));
    const commitFilters = filter.ops.length > 0 || filter.users.length > 0;
    const wantState = (s: SessionState) => filter.states.length === 0 || filter.states.includes(s);
    const newestFirst = (cs: readonly CommitRec[]) => [...cs].reverse();
    const users = [...new Set(log.commits.map((c) => c.user).filter((u): u is string => u !== null))].sort();

    if (!options.grouped) {
        const stateById = new Map(log.sessions.map((s) => [s.id, stateOf(s, log.activeSessionId)]));
        const all = newestFirst(log.commits).filter((c) => {
            const state = c.session !== null ? stateById.get(c.session) ?? "none" : "none";
            return wantState(state) && passes(c) && commitText(c, t);
        });
        return { sessions: [], commits: all.map(row), users };
    }

    const rows: SessionRow[] = [];
    for (const s of [...log.sessions].reverse()) {
        const state = stateOf(s, log.activeSessionId);
        if (!wantState(state)) continue;
        const self = sessionText(s, t);
        const shown = newestFirst(s.commits).filter((c) => passes(c) && (self || commitText(c, t)));
        const emptyButStarted =
            s.commits.length === 0 && !commitFilters && inRange(s.ts) && (t.q === "" || self);
        if (shown.length === 0 && !emptyButStarted) continue;
        rows.push({
            id: s.id,
            summary: summaryLine(s.msg),
            msg: s.msg,
            ticket: s.ticket,
            branch: log.branchAt !== null && s.recIndex > log.branchAt ? log.branchName : null,
            ts: s.ts,
            endTs: s.endTs,
            state,
            commits: shown.map(row),
            total: s.commits.length,
            matchedSelf: self,
        });
    }
    if (wantState("none") && log.noSession.length > 0) {
        const shown = newestFirst(log.noSession).filter((c) => passes(c) && commitText(c, t));
        if (shown.length > 0) {
            const last = log.noSession[log.noSession.length - 1];
            rows.push({
                id: null,
                summary: "no session",
                msg: "Commits recorded with --no-session",
                ticket: null,
                branch: null,
                ts: last.ts,
                endTs: null,
                state: "none",
                commits: shown.map(row),
                total: log.noSession.length,
                matchedSelf: false,
            });
        }
    }
    return { sessions: rows, commits: [], users };
}

function paged(
    grouped: boolean,
    sessions: SessionRow[],
    commits: CommitRow[],
    page: number,
    pageSize: number,
    users: readonly string[],
): HistoryPage {
    const total = grouped ? sessions.length : commits.length;
    const pages = Math.max(1, Math.ceil(total / pageSize));
    const p = Math.min(Math.max(0, Math.floor(page)), pages - 1);
    const from = p * pageSize;
    return {
        grouped,
        sessions: grouped ? sessions.slice(from, from + pageSize) : [],
        commits: grouped ? [] : commits.slice(from, from + pageSize),
        page: p,
        pages,
        pageSize,
        total,
        users,
    };
}
