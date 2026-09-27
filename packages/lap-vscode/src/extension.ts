/* lap-vscode: visualization-only view of a lap repository.
 * Reads .lap/log.jsonl directly and refreshes live through a file watcher.
 * See cli/lap-cli/SPEC.md for the record schema. The CLI is asked only to
 * resolve a reference to a commit (`lap show <ref> --json`); where it cannot
 * answer, the log's own hashes do.
 */

import { execFile } from "child_process";
import * as crypto from "crypto";
import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

import { EMPTY_FILTER, HistoryFilter, pageOf, query } from "./history";
import {
    CommitRec,
    LapLog,
    LogReader,
    SessionRec,
    consumableBytes,
    createReader,
    mdEscape,
    readerFeed,
    regionLabel,
    replaySeeded,
    shortHash,
    stateText,
    summaryLine,
} from "./model";
import type { ToHost, ToView } from "./protocol";
import { Resolved, mdLinked, resolveRef } from "./refs";

const GROUPING_KEY = "lap.groupBySession";
const STATE_SCHEME = "lap-state";

interface Repo {
    root: string; /* workspace folder containing .lap */
    logPath: string;
}

function findRepo(): Repo | undefined {
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        const root = folder.uri.fsPath;
        const logPath = path.join(root, ".lap", "log.jsonl");
        if (fs.existsSync(logPath)) {
            return { root, logPath };
        }
    }
    return undefined;
}

/* A record's hash: the SHA-256 of its line's exact bytes. */
function lineHash(line: string): string {
    return crypto.createHash("sha256").update(line, "utf8").digest("hex");
}

/* The log, read incrementally as it grows. */
class LapLogSource {
    private repo: Repo | undefined;
    private reader: LogReader = createReader(lineHash);
    private offset = 0;

    constructor(private readonly state: vscode.Memento) {
        this.refresh();
    }

    get groupBySession(): boolean {
        return this.state.get<boolean>(GROUPING_KEY, true);
    }

    async toggleGrouping(): Promise<void> {
        await this.state.update(GROUPING_KEY, !this.groupBySession);
    }

    private reset(repo: Repo | undefined): void {
        this.repo = repo;
        this.reader = createReader(lineHash);
        this.offset = 0;
    }

    /* Incremental: read only the bytes appended since last time.
     *
     * `offset` never advances past a newline, so it always names a record
     * boundary. That is what makes the incremental read safe against the
     * CLI's torn-tail repair: a crash leaves a partial record, the next
     * writer truncates it away and appends a real one, and because we
     * never consumed those bytes we simply read the replacement. Buffering
     * partial bytes instead would leave us reading from mid-record
     * forever. It also makes a short read harmless — unconsumed bytes are
     * just re-read — and keeps multi-byte UTF-8 whole. */
    refresh(): void {
        const found = findRepo();
        if (!found || found.root !== this.repo?.root) {
            this.reset(found);
        }
        if (this.repo) {
            try {
                const size = fs.statSync(this.repo.logPath).size;
                if (size < this.offset) {
                    this.reset(this.repo); /* truncated: reparse */
                }
                if (size > this.offset) {
                    const fd = fs.openSync(this.repo.logPath, "r");
                    const buf = Buffer.alloc(size - this.offset);
                    let got = 0;
                    for (;;) {
                        const n = fs.readSync(
                            fd,
                            buf,
                            got,
                            buf.length - got,
                            this.offset + got,
                        );
                        if (n <= 0 || got + n >= buf.length) {
                            got += Math.max(n, 0);
                            break;
                        }
                        got += n;
                    }
                    fs.closeSync(fd);
                    const take = consumableBytes(buf, got);
                    if (take > 0) {
                        readerFeed(
                            this.reader,
                            buf.subarray(0, take).toString("utf8"),
                        );
                        this.offset += take;
                    }
                }
            } catch {
                this.reset(this.repo);
            }
        }
    }

    get current(): LapLog {
        return this.reader.log;
    }

    get repoRoot(): string | undefined {
        return this.repo?.root;
    }

    get hasRepo(): boolean {
        return this.repo !== undefined;
    }

    /* The commit a reference names, as `lap show` resolves it. When lap is
     * not installed, is too old to know hashes, or does not answer, the log
     * read here resolves it by the same rules. */
    resolve(ref: string): Promise<Resolved> {
        const local = () => resolveRef(this.current, ref);
        const root = this.repo?.root;
        if (!root) return Promise.resolve(local());
        return new Promise((done) => {
            execFile("lap", ["show", ref, "--json"], { cwd: root, timeout: 10_000, maxBuffer: 64 * 1024 * 1024 }, (_err, stdout) => {
                try {
                    const r = JSON.parse(String(stdout)) as { ok?: unknown; id?: unknown };
                    if (r.ok === true && typeof r.id === "string") {
                        done({ ok: true, id: r.id });
                        return;
                    }
                } catch {
                    /* no answer: the log decides */
                }
                done(local());
            });
        });
    }
}

/* The History view: a webview, because a native tree cannot hold the filter
 * bar. The view says what is set; the host runs the query over the log
 * (history.ts) and sends back one page, so the log's edit text never
 * crosses. */
class HistoryView implements vscode.WebviewViewProvider {
    private view: vscode.WebviewView | null = null;
    private filter: HistoryFilter = EMPTY_FILTER;
    private page = 0;
    /* A commit to reveal once the view, not yet shown, first asks. */
    private pending: string | null = null;

    constructor(
        private readonly extensionUri: vscode.Uri,
        private readonly source: LapLogSource,
        private readonly onOpen: (id: string) => void,
    ) {}

    resolveWebviewView(view: vscode.WebviewView): void {
        this.view = view;
        const media = vscode.Uri.joinPath(this.extensionUri, "out", "media");
        view.webview.options = { enableScripts: true, localResourceRoots: [media] };
        view.webview.html = historyHtml(view.webview, media);
        view.webview.onDidReceiveMessage((m: ToHost) => {
            if (m.type === "query") {
                if (this.pending !== null) {
                    /* the filter and page were chosen for the commit */
                    this.push(this.pending);
                    this.pending = null;
                    return;
                }
                this.filter = m.filter;
                this.page = m.page;
                this.push();
            } else if (m.type === "open") {
                this.onOpen(m.id);
            } else if (m.type === "reveal") {
                void this.reveal(m.ref);
            }
        });
        view.onDidDispose(() => {
            this.view = null;
        });
    }

    /* The page for the view's last query, against the log as it is now;
     * with `reveal`, the page was chosen to show that commit. */
    push(reveal: string | null = null): void {
        if (!this.view) return;
        const log = this.source.current;
        const msg: ToView = this.source.hasRepo
            ? {
                  type: "page",
                  page: query(log, this.filter, { grouped: this.source.groupBySession, page: this.page, now: new Date() }),
                  hasRepo: true,
                  active: log.activeSessionId,
                  reveal: reveal === null ? null : { id: reveal, filter: this.filter },
              }
            : { type: "page", page: null, hasRepo: false, active: null, reveal: null };
        void this.view.webview.postMessage(msg);
    }

    /* Shows the commit a reference names: on its page under the view's
     * filter, or under All with nothing else set when the filter hides it. */
    async reveal(ref: string): Promise<void> {
        const r = await this.source.resolve(ref);
        if (!r.ok) {
            void vscode.window.showWarningMessage(
                r.error === "ambiguous_ref"
                    ? `lap: ${ref} names more than one commit (${r.matches.join(", ")})`
                    : `lap: no commit is named ${ref}`,
            );
            return;
        }
        const log = this.source.current;
        const at = { grouped: this.source.groupBySession, now: new Date() };
        let page = pageOf(log, this.filter, at, r.id);
        if (page === null) {
            this.filter = { ...EMPTY_FILTER, range: "all" };
            page = pageOf(log, this.filter, at, r.id);
        }
        if (page === null) {
            void vscode.window.showWarningMessage(`lap: commit ${r.id} is not in the log yet`);
            return;
        }
        this.page = page;
        if (this.view) {
            this.push(r.id);
            this.view.show?.(true);
        } else {
            this.pending = r.id;
            await vscode.commands.executeCommand("lapHistory.focus");
        }
    }

    collapseAll(): void {
        void this.view?.webview.postMessage({ type: "collapseAll" } satisfies ToView);
    }
}

function historyHtml(webview: vscode.Webview, media: vscode.Uri): string {
    const nonce = crypto.randomBytes(16).toString("base64");
    const uri = (f: string) => webview.asWebviewUri(vscode.Uri.joinPath(media, f)).toString();
    const csp = [
        "default-src 'none'",
        `style-src ${webview.cspSource} 'unsafe-inline'`,
        `script-src 'nonce-${nonce}'`,
        `font-src ${webview.cspSource}`,
    ].join("; ");
    const css = ["baukasten-base.css", "baukasten-vscode.css", "codicon.css", "lap.css"]
        .map((f) => `<link rel="stylesheet" href="${uri(f)}">`)
        .join("\n");
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="${csp}">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
${css}
</head>
<body>
<div id="root"></div>
<script nonce="${nonce}" src="${uri("lap-history.js")}"></script>
</body>
</html>`;
}

/* Serves a file's replayed content at "before"/"after" a commit, so the
 * built-in (Monaco) diff editor can render the change natively. URI shape:
 * lap-state:/<commit-id>/<side>/<repo-relative-file-path> — ending with the
 * real filename keeps VSCode's language detection working inside the diff.
 */
class StateContentProvider implements vscode.TextDocumentContentProvider {
    private readonly emitter = new vscode.EventEmitter<vscode.Uri>();
    readonly onDidChange = this.emitter.event;

    constructor(private readonly provider: LapLogSource) {}

    provideTextDocumentContent(uri: vscode.Uri): string {
        const parts = uri.path.replace(/^\//, "").split("/");
        if (parts.length < 3) {
            return "";
        }
        const id = parts[0];
        const side = parts[1];
        const file = parts.slice(2).join("/");
        const log = this.provider.current;
        const target = log.commits.find((c) => c.id === id);
        if (!target) {
            return "";
        }
        let snapshots: string | null = null;
        const root = this.provider.repoRoot;
        if (root) {
            try {
                snapshots = fs.readFileSync(
                    path.join(root, ".lap", "snapshots", `${file}.jsonl`),
                    "utf8",
                );
            } catch {
                /* no snapshots: replay from birth, same result */
            }
        }
        const upto =
            side === "before" ? target.recIndex - 1 : target.recIndex;
        return stateText(replaySeeded(log, file, upto, snapshots));
    }
}

function stateUri(id: string, side: "before" | "after", file: string) {
    return vscode.Uri.from({
        scheme: STATE_SCHEME,
        path: `/${id}/${side}/${file}`,
    });
}

/* The record's UTC timestamp rendered in the viewer's local timezone as
 * "<date>:<time>", e.g. "09/20/2026:10:51 PM". */
function localTime(iso: string): string {
    const d = new Date(iso);
    if (isNaN(d.getTime())) {
        return iso;
    }
    const date = d.toLocaleDateString(undefined, {
        year: "numeric",
        month: "2-digit",
        day: "2-digit",
    });
    const time = d.toLocaleTimeString(undefined, {
        hour: "2-digit",
        minute: "2-digit",
    });
    return `${date}:${time}`;
}

/* The changed region in the after-document, 0-based. Deletions anchor on
 * the line where the removal happened. */
function commitRange(c: CommitRec): vscode.Range {
    const start = Math.max(0, c.newStart - 1);
    const end = c.newLines > 0 ? start + c.newLines - 1 : start;
    return new vscode.Range(start, 0, end, 0);
}

/* Attaches each commit's description INSIDE the native diff editor as an
 * inline comment thread anchored at the changed lines — the same UX review
 * extensions use for conversations on diffs. Real editors render all code,
 * so syntax highlighting comes for free everywhere.
 */
class CommitComments {
    private readonly controller = vscode.comments.createCommentController(
        "lap",
        "Lap",
    );
    private readonly threads = new Map<string, vscode.CommentThread>();

    constructor(context: vscode.ExtensionContext) {
        context.subscriptions.push(this.controller);
    }

    show(
        commit: CommitRec,
        session: SessionRec | undefined,
        afterUri: vscode.Uri,
    ): void {
        const existing = this.threads.get(commit.id);
        if (existing) {
            existing.collapsibleState =
                vscode.CommentThreadCollapsibleState.Expanded;
            return;
        }
        /* Layout: "[id] [short hash] @ [local time] [user]:" as the author
         * line, then the intent, the behavior, whether the message checks
         * were skipped, a rule, and the session (muted via italics). A
         * trailing no-break-space paragraph keeps the editor's scrollbar
         * from overlapping the last text line. References to other commits
         * are links that reveal them in the History view. */
        /* never appendText here: it turns spaces into &nbsp; and kills
         * word-wrap (see mdProse/mdEscape in model.ts) */
        const body = new vscode.MarkdownString();
        body.isTrusted = { enabledCommands: ["lap.revealCommit"] };
        const link = (ref: string) =>
            `command:lap.revealCommit?${encodeURIComponent(JSON.stringify([ref]))}`;
        body.appendMarkdown(`**Intent**  \n${mdLinked(commit.intent, link)}`);
        body.appendMarkdown(`\n\n**Behavior**  \n${mdLinked(commit.behavior, link)}`);
        if (commit.forced) {
            body.appendMarkdown(
                `\n\n*${mdEscape("forced: the message checks were skipped (--force-message)")}*`,
            );
        }
        const footer = commit.session
            ? `session ${commit.session}` +
              (session ? `: ${summaryLine(session.msg)}` : "")
            : "committed outside any session (--no-session)";
        body.appendMarkdown(`\n\n---\n\n*${mdEscape(footer)}*`);
        body.appendMarkdown("\n\n&nbsp;");
        const comment: vscode.Comment = {
            author: {
                name:
                    `${commit.id} ${shortHash(commit.hash)} @ ${localTime(commit.ts)}` +
                    (commit.user ? ` ${commit.user}` : "") +
                    ":",
            },
            body,
            mode: vscode.CommentMode.Preview,
        };
        const thread = this.controller.createCommentThread(
            afterUri,
            commitRange(commit),
            [comment],
        );
        thread.canReply = false;
        thread.collapsibleState =
            vscode.CommentThreadCollapsibleState.Expanded;
        thread.label = `${commit.file} · ${regionLabel(commit)}`;
        this.threads.set(commit.id, thread);
    }
}

async function openCommitDiff(
    commit: CommitRec,
    session: SessionRec | undefined,
    comments: CommitComments,
): Promise<void> {
    const before = stateUri(commit.id, "before", commit.file);
    const after = stateUri(commit.id, "after", commit.file);
    await vscode.commands.executeCommand(
        "vscode.diff",
        before,
        after,
        `${commit.id} ${shortHash(commit.hash)} · ${commit.file}`,
        {
            preview: true,
            selection: commitRange(commit),
        } satisfies vscode.TextDocumentShowOptions,
    );
    comments.show(commit, session, after);
}

export function activate(context: vscode.ExtensionContext): void {
    const tree = new LapLogSource(context.workspaceState);
    const states = new StateContentProvider(tree);
    context.subscriptions.push(
        vscode.workspace.registerTextDocumentContentProvider(
            STATE_SCHEME,
            states,
        ),
    );

    const comments = new CommitComments(context);
    const showCommitDiff = (id: string): void => {
        const log = tree.current;
        const commit = log.commits.find((c) => c.id === id);
        if (!commit) {
            void vscode.window.showWarningMessage(
                `lap: commit ${id} not found in the current log`,
            );
            return;
        }
        const session = commit.session
            ? log.sessions.find((s) => s.id === commit.session)
            : undefined;
        void openCommitDiff(commit, session, comments);
    };

    const history = new HistoryView(context.extensionUri, tree, showCommitDiff);
    context.subscriptions.push(vscode.window.registerWebviewViewProvider("lapHistory", history));

    const status = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Left,
        50,
    );
    status.command = "lapHistory.focus";
    context.subscriptions.push(status);

    const updateStatus = (): void => {
        if (!tree.hasRepo) {
            status.hide();
            return;
        }
        const log = tree.current;
        if (log.activeSessionId) {
            const s = log.sessions.find(
                (x) => x.id === log.activeSessionId,
            );
            status.text = `$(pulse) lap ${log.activeSessionId}`;
            status.tooltip = s
                ? `active lap session ${s.id}: ${summaryLine(s.msg)}`
                : "active lap session";
        } else {
            status.text = "$(circle-outline) lap";
            status.tooltip = "lap: no active session";
        }
        status.show();
    };
    updateStatus();

    let refreshTimer: NodeJS.Timeout | undefined;
    const scheduleRefresh = (): void => {
        if (refreshTimer) {
            clearTimeout(refreshTimer);
        }
        refreshTimer = setTimeout(() => {
            tree.refresh();
            history.push();
            updateStatus();
        }, 200);
    };

    const watcher = vscode.workspace.createFileSystemWatcher(
        "**/.lap/log.jsonl",
    );
    watcher.onDidChange(scheduleRefresh);
    watcher.onDidCreate(scheduleRefresh);
    watcher.onDidDelete(scheduleRefresh);
    context.subscriptions.push(watcher);
    context.subscriptions.push(
        vscode.workspace.onDidChangeWorkspaceFolders(scheduleRefresh),
    );

    context.subscriptions.push(
        vscode.commands.registerCommand("lap.refresh", () => {
            tree.refresh();
            history.push();
            updateStatus();
        }),
        vscode.commands.registerCommand("lap.toggleGrouping", async () => {
            await tree.toggleGrouping();
            history.push();
        }),
        vscode.commands.registerCommand("lap.collapseAll", () => {
            history.collapseAll();
        }),
        vscode.commands.registerCommand("lap.showCommit", (id: string) => {
            showCommitDiff(id);
        }),
        vscode.commands.registerCommand("lap.revealCommit", (ref: string) =>
            history.reveal(ref),
        ),
    );
}

export function deactivate(): void {
    /* everything lives in context.subscriptions */
}
