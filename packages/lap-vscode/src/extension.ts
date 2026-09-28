/* lap-vscode: visualization-only view of a lap repository.
 * Reads the history's files in .lap/ directly (chunks.ts) and refreshes
 * live through a file watcher.
 * See cli/lap-cli/SPEC.md for the record schema. The CLI is asked only to
 * resolve a reference to a commit (`lap show <ref> --json`); where it cannot
 * answer, the log's own hashes do.
 */

import { execFile } from "child_process";
import * as crypto from "crypto";
import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

import { askBranchList, BranchRow, BranchView, branchView, LapExec, lapBin, ListGate, parseBranchList } from "./branches";
import { IncrementalLog, branchProblem, folderFiles, hasHistory, historyProblem, ownFiles, parseChunkName, readStream } from "./chunks";
import { EMPTY_FILTER, HistoryFilter, pageOf, query } from "./history";
import {
    CommitRec,
    LapLog,
    LogReader,
    SessionRec,
    consumableBytes,
    createReader,
    localTime,
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
    lapDir: string;
}

function findRepo(): Repo | undefined {
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        const root = folder.uri.fsPath;
        const lapDir = path.join(root, ".lap");
        if (hasHistory(lapDir)) {
            return { root, lapDir };
        }
    }
    return undefined;
}

/* The lap to run, as the settings say (lap.path, else coboard.lapPath). */
function lap(): string {
    return lapBin(
        vscode.workspace.getConfiguration("lap").get<string>("path"),
        vscode.workspace.getConfiguration("coboard").get<string>("lapPath"),
    );
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
    /* why the history cannot be shown (a missing chunk), or null */
    problem: string | null = null;

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

    /* Incremental: read only the bytes appended since last time. The
     * offset is into the history's files read as one stream; sealed chunks
     * never change, so only the open chunk's new bytes are read.
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
            /* a broken history is named, never shown in part */
            this.problem = historyProblem(this.repo.lapDir);
            if (this.problem) {
                this.reader = createReader(lineHash);
                this.offset = 0;
                return;
            }
            try {
                const files = folderFiles(this.repo.lapDir);
                const size = files.reduce((n, f) => n + f.size, 0);
                if (size < this.offset) {
                    this.reset(this.repo); /* truncated: reparse */
                }
                if (size > this.offset) {
                    const buf = readStream(files, this.offset, size);
                    const take = consumableBytes(buf, buf.length);
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
            execFile(lap(), ["show", ref, "--json"], { cwd: root, timeout: 10_000, maxBuffer: 64 * 1024 * 1024 }, (_err, stdout) => {
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

/* The branches this folder started: what `lap branch list --json` says, each
 * with its own sessions, read from its folder while that is there and from
 * its chunks here after. lap runs asynchronously, so the view shows what was
 * last known and gets a fresh page when lap answers. A branch's log is read
 * again only when its files grew. Registered folders are watched, so work in
 * them shows here as it happens. */
/* What `lap branch list` depends on in this folder, as one string: the
 * registry file's stat, how many merges its history records, and the
 * branch chunks here with their sizes. "" when there is none of it. */
function listSignature(root: string, merges: number): string {
    const parts: string[] = [];
    try {
        const st = fs.statSync(path.join(root, ".lap", "branches.json"));
        parts.push(`registry ${st.mtimeMs} ${st.size}`);
    } catch {
        /* no registry */
    }
    if (merges > 0) parts.push(`merges ${merges}`);
    let names: string[] = [];
    try {
        names = fs.readdirSync(path.join(root, ".lap", "log"));
    } catch {
        /* no chunks */
    }
    for (const n of names.sort()) {
        const c = parseChunkName(n);
        if (!c || c.lineage === "main") continue;
        try {
            parts.push(`${n} ${fs.statSync(path.join(root, ".lap", "log", n)).size}`);
        } catch {
            /* gone meanwhile */
        }
    }
    return parts.join("\n");
}

class BranchSource {
    views: BranchView[] = [];
    /* why lap gave no branch list, in its words; null when it answered */
    error: string | null = null;
    private logs = new Map<string, IncrementalLog>();
    private gate = new ListGate();
    private watchers: vscode.FileSystemWatcher[] = [];
    private watched = "";
    private timer: NodeJS.Timeout | undefined;

    constructor(
        private readonly source: LapLogSource,
        private readonly onChange: () => void,
    ) {}

    /* Asks lap for the list again only when something it depends on moved
     * — the registry, this folder's merges, the branch chunks here — or
     * when told to (a registered folder changed, the registry was fixed):
     * a folder with no branches never runs lap at all. */
    refresh(force = false): void {
        const root = this.source.repoRoot;
        if (!root) {
            this.views = [];
            return;
        }
        const sig = listSignature(root, this.source.current.merges.length);
        if (sig === "") { /* no registry, no branch chunk, no merge */
            if (!this.gate.ask(sig, force)) return;
            this.gate.done(sig, true);
            this.error = null;
            if (this.views.length) {
                this.views = [];
                this.onChange();
            }
            return;
        }
        const exec: LapExec = (args, cwd, cb) =>
            execFile(lap(), args, { cwd, timeout: 10_000, maxBuffer: 16 * 1024 * 1024 }, (err, stdout) => cb(err, String(stdout)));
        askBranchList(this.gate, sig, force, root, exec, (error, out) => {
            /* this folder has branches, so no list is a failure to show */
            this.error = error;
            const rows = parseBranchList(out, this.source.current);
            this.views = rows.map((r) => {
                const problem = branchProblem(this.lapDirOf(root, r), r.id);
                return branchView(r, problem ? null : this.read(root, r), new Date(), problem);
            });
            this.watch(rows);
            this.onChange();
        });
    }

    /* A branch's own records, from its folder while it is there and from
     * its chunks here after: read as they grow, never again from the start
     * (the history it started from is main's, read already). */
    /* where a branch's history is read: its folder while it is there,
     * else its chunks here */
    private lapDirOf(root: string, r: BranchRow): string {
        return r.present ? path.join(r.path, ".lap") : path.join(root, ".lap");
    }

    private read(root: string, r: BranchRow): LapLog | null {
        const lapDir = this.lapDirOf(root, r);
        let log = this.logs.get(r.id);
        if (!log) {
            log = new IncrementalLog(lineHash);
            this.logs.set(r.id, log);
        }
        return log.update(ownFiles(lapDir, r.id));
    }

    private watch(rows: readonly BranchRow[]): void {
        const paths = rows.filter((r) => r.present).map((r) => r.path);
        if (paths.join("\n") === this.watched) return;
        this.dispose();
        this.watched = paths.join("\n");
        for (const p of paths) {
            const w = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(vscode.Uri.file(p), ".lap/log/*.jsonl"));
            const later = () => {
                if (this.timer) clearTimeout(this.timer);
                this.timer = setTimeout(() => this.refresh(true), 300);
            };
            w.onDidChange(later);
            w.onDidCreate(later);
            this.watchers.push(w);
        }
    }

    dispose(): void {
        for (const w of this.watchers) w.dispose();
        this.watchers = [];
        this.watched = "";
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
        private readonly branches: { readonly views: readonly BranchView[]; readonly error: string | null; refresh(force?: boolean): void },
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
            } else if (m.type === "original") {
                this.original(m.hash);
            } else if (m.type === "branchFix") {
                void this.fixBranch(m.action, m.name);
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
                  branches: this.branches.views,
                  problem: this.source.problem,
                  lapError: this.branches.error,
              }
            : { type: "page", page: null, hasRepo: false, active: null, reveal: null, branches: [], problem: null, lapError: null };
        void this.view.webview.postMessage(msg);
    }

    /* An adopted commit's original, as `lap show` prints it (lap finds a
     * hash in the branches whose chunks are here). */
    private original(hash: string): void {
        const root = this.source.repoRoot;
        if (!root) return;
        execFile(lap(), ["show", hash, "--color=never"], { cwd: root, timeout: 10_000, maxBuffer: 64 * 1024 * 1024 }, (err, stdout) => {
            if (err && !stdout) {
                void vscode.window.showWarningMessage(`lap: the original ${hash.slice(0, 7)} could not be shown: ${err.message}`);
                return;
            }
            void vscode.workspace
                .openTextDocument({ content: String(stdout), language: "plaintext" })
                .then((doc) => vscode.window.showTextDocument(doc, { preview: true }));
        });
    }

    /* A missing branch's two fixes: point it at the folder it moved to, or
     * drop it from the registry. */
    private async fixBranch(action: "move" | "forget", name: string): Promise<void> {
        const root = this.source.repoRoot;
        if (!root) return;
        let args: string[];
        if (action === "forget") {
            const ok = await vscode.window.showWarningMessage(
                `Forget branch ${name}? Its folder is gone; what was not merged stays only in its chunks, if git carried them.`,
                { modal: true },
                "Forget",
            );
            if (ok !== "Forget") return;
            args = ["branch", "forget", name, "--json"];
        } else {
            const picked = await vscode.window.showOpenDialog({ canSelectFolders: true, canSelectFiles: false, openLabel: `Branch ${name} is here` });
            if (!picked || picked.length === 0) return;
            args = ["branch", "move", name, picked[0].fsPath, "--json"];
        }
        execFile(lap(), args, { cwd: root, timeout: 10_000 }, (_err, stdout) => {
            try {
                const r = JSON.parse(String(stdout)) as { ok?: boolean; message?: string };
                if (r.ok !== true) void vscode.window.showErrorMessage(`lap: ${r.message ?? "the registry was not changed"}`);
            } catch {
                void vscode.window.showErrorMessage("lap: the registry was not changed (is lap installed?)");
            }
            this.branches.refresh(true);
        });
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
        if (commit.earlier.length) {
            const n = commit.earlier.length;
            body.appendMarkdown(
                `\n\n*${mdEscape(`amended ${n === 1 ? "once" : `${n} times`} (lap amend): Lap History shows the earlier text${n === 1 ? "" : "s"}`)}*`,
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

    /* the History view is made after the branches it shows, which push to
     * it when lap answers */
    let pushPage = (): void => {};
    const branches = new BranchSource(tree, () => pushPage());
    context.subscriptions.push(branches);
    const history = new HistoryView(context.extensionUri, tree, showCommitDiff, branches);
    pushPage = () => history.push();
    context.subscriptions.push(vscode.window.registerWebviewViewProvider("lapHistory", history));
    branches.refresh(true);

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
            branches.refresh();
        }, 200);
    };

    const watcher = vscode.workspace.createFileSystemWatcher(
        "**/.lap/{log.jsonl,log/*.jsonl}",
    );
    watcher.onDidChange(scheduleRefresh);
    watcher.onDidCreate(scheduleRefresh);
    watcher.onDidDelete(scheduleRefresh);
    context.subscriptions.push(watcher);
    context.subscriptions.push(
        vscode.workspace.onDidChangeWorkspaceFolders(scheduleRefresh),
        /* another lap: everything it said is asked again */
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (!e.affectsConfiguration("lap.path") && !e.affectsConfiguration("coboard.lapPath")) return;
            tree.refresh();
            history.push();
            updateStatus();
            branches.refresh(true);
        }),
    );

    context.subscriptions.push(
        vscode.commands.registerCommand("lap.refresh", () => {
            tree.refresh();
            history.push();
            updateStatus();
            branches.refresh(true);
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
