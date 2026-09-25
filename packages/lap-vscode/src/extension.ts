/* lap-vscode: visualization-only view of a lap repository.
 * Reads .lap/log.jsonl directly (no CLI dependency at runtime) and refreshes
 * live through a file watcher. See cli/lap-cli/SPEC.md for the record schema.
 */

import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

import {
    CommitRec,
    LapLog,
    LogReader,
    SessionRec,
    consumableBytes,
    createReader,
    mdEscape,
    mdProse,
    readerFeed,
    regionLabel,
    replaySeeded,
    stateText,
    summaryLine,
} from "./model";

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


type Node =
    | { type: "session"; session: SessionRec; active: boolean }
    | { type: "no-session-group"; commits: CommitRec[] }
    | { type: "commit"; commit: CommitRec };

class LapTreeProvider implements vscode.TreeDataProvider<Node> {
    private readonly emitter = new vscode.EventEmitter<Node | undefined>();
    readonly onDidChangeTreeData = this.emitter.event;

    private repo: Repo | undefined;
    private reader: LogReader = createReader();
    private offset = 0;

    constructor(private readonly state: vscode.Memento) {
        this.refresh();
    }

    get groupBySession(): boolean {
        return this.state.get<boolean>(GROUPING_KEY, true);
    }

    async toggleGrouping(): Promise<void> {
        await this.state.update(GROUPING_KEY, !this.groupBySession);
        this.emitter.fire(undefined);
    }

    private reset(repo: Repo | undefined): void {
        this.repo = repo;
        this.reader = createReader();
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
        this.emitter.fire(undefined);
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

    getChildren(element?: Node): Node[] {
        if (!element) {
            if (!this.repo) {
                return []; /* viewsWelcome takes over */
            }
            if (!this.groupBySession) {
                return [...this.reader.log.commits]
                    .reverse()
                    .map((commit) => ({ type: "commit", commit }));
            }
            const roots: Node[] = [...this.reader.log.sessions]
                .reverse()
                .map((session) => ({
                    type: "session",
                    session,
                    active: session.id === this.reader.log.activeSessionId,
                }));
            if (this.reader.log.noSession.length > 0) {
                roots.push({
                    type: "no-session-group",
                    commits: this.reader.log.noSession,
                });
            }
            return roots;
        }
        if (element.type === "session") {
            return [...element.session.commits]
                .reverse()
                .map((commit) => ({ type: "commit", commit }));
        }
        if (element.type === "no-session-group") {
            return [...element.commits]
                .reverse()
                .map((commit) => ({ type: "commit", commit }));
        }
        return [];
    }

    getTreeItem(node: Node): vscode.TreeItem {
        if (node.type === "session") {
            const s = node.session;
            const item = new vscode.TreeItem(
                `${s.id}  ${summaryLine(s.msg)}`,
                node.active
                    ? vscode.TreeItemCollapsibleState.Expanded
                    : vscode.TreeItemCollapsibleState.Collapsed,
            );
            const n = s.commits.length;
            item.description = `${n} commit${n === 1 ? "" : "s"}${
                node.active ? " • active" : ""
            }`;
            item.iconPath = new vscode.ThemeIcon(
                node.active ? "play-circle" : "milestone",
            );
            item.tooltip = new vscode.MarkdownString(
                `**${s.id}** — ${n} commit${n === 1 ? "" : "s"}\n\n` +
                    `${s.msg}\n\n` +
                    `started ${s.ts}` +
                    (s.endTs
                        ? `, ended ${s.endTs}`
                        : node.active
                          ? " — **active**"
                          : " — open"),
            );
            item.contextValue = "lapSession";
            return item;
        }
        if (node.type === "no-session-group") {
            const n = node.commits.length;
            const item = new vscode.TreeItem(
                "no session",
                vscode.TreeItemCollapsibleState.Collapsed,
            );
            item.description = `${n} commit${n === 1 ? "" : "s"}`;
            item.iconPath = new vscode.ThemeIcon("circle-slash");
            item.tooltip = "Commits recorded with --no-session";
            return item;
        }
        const c = node.commit;
        const item = new vscode.TreeItem(
            `${c.id}  ${summaryLine(c.msg)}`,
            vscode.TreeItemCollapsibleState.None,
        );
        item.description = `${c.file} · ${regionLabel(c)}`;
        const icon =
            c.op === "create"
                ? "diff-added"
                : c.op === "delete"
                  ? "diff-removed"
                  : "diff-modified";
        item.iconPath = new vscode.ThemeIcon(icon);
        const tooltip = new vscode.MarkdownString();
        tooltip.appendMarkdown(
            `**${c.id}** · ${c.file} · ${regionLabel(c)} · ${c.ts}` +
                (c.session ? ` · session ${c.session}` : " · no session") +
                "\n\n",
        );
        tooltip.appendText(c.msg);
        item.tooltip = tooltip;
        item.command = {
            command: "lap.showCommit",
            title: "Show Commit",
            arguments: [c.id],
        };
        item.contextValue = "lapCommit";
        return item;
    }
}

/* Serves a file's replayed content at "before"/"after" a commit, so the
 * built-in (Monaco) diff editor can render the change natively. URI shape:
 * lap-state:/<commit-id>/<side>/<repo-relative-file-path> — ending with the
 * real filename keeps VSCode's language detection working inside the diff.
 */
class StateContentProvider implements vscode.TextDocumentContentProvider {
    private readonly emitter = new vscode.EventEmitter<vscode.Uri>();
    readonly onDidChange = this.emitter.event;

    constructor(private readonly provider: LapTreeProvider) {}

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
        /* Layout: "[id]/[local time] [user]:" as the author line, then the
         * message body, a rule, and the session (muted via italics). A
         * trailing no-break-space paragraph keeps the editor's scrollbar
         * from overlapping the last text line. */
        /* never appendText here: it turns spaces into &nbsp; and kills
         * word-wrap (see mdProse/mdEscape in model.ts) */
        const body = new vscode.MarkdownString();
        body.appendMarkdown(mdProse(commit.msg));
        const footer = commit.session
            ? `session ${commit.session}` +
              (session ? `: ${summaryLine(session.msg)}` : "")
            : "committed outside any session (--no-session)";
        body.appendMarkdown(`\n\n---\n\n*${mdEscape(footer)}*`);
        body.appendMarkdown("\n\n&nbsp;");
        const comment: vscode.Comment = {
            author: {
                name:
                    `${commit.id} @ ${localTime(commit.ts)}` +
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
        `${commit.id} · ${commit.file}`,
        {
            preview: true,
            selection: commitRange(commit),
        } satisfies vscode.TextDocumentShowOptions,
    );
    comments.show(commit, session, after);
}

export function activate(context: vscode.ExtensionContext): void {
    const tree = new LapTreeProvider(context.workspaceState);
    const view = vscode.window.createTreeView("lapHistory", {
        treeDataProvider: tree,
        showCollapseAll: true,
    });
    context.subscriptions.push(view);

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
            updateStatus();
        }),
        vscode.commands.registerCommand("lap.toggleGrouping", async () => {
            await tree.toggleGrouping();
        }),
        vscode.commands.registerCommand("lap.showCommit", (id: string) => {
            showCommitDiff(id);
        }),
    );
}

export function deactivate(): void {
    /* everything lives in context.subscriptions */
}
