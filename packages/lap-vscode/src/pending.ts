/* The badge on Lap History: how many files hold edits lap has not recorded
 * yet, as git's badge counts changed files. VS Code shows it on the view's
 * activity-bar icon and shortens big numbers itself (1K+).
 *
 * The count is `lap status --json`'s files. Asking costs a scan of the
 * tracked files (about a third of a second on a large repository), so a
 * change only schedules a recount after things settle, and one count runs at
 * a time: changes that arrive while it runs ask for one more count after it,
 * never a queue of them. Pure but for the clock: the host hands in how to
 * ask lap, and the tests a fake.
 */

export interface Badge {
    readonly value: number;
    readonly tooltip: string;
}

/* The number of files with pending edits in `lap status --json`'s answer;
 * null when it is not one (lap missing, too old, or refused). */
export function pendingCount(stdout: string): number | null {
    try {
        const v = JSON.parse(stdout) as { ok?: unknown; files?: unknown };
        return v.ok === true && Array.isArray(v.files) ? v.files.length : null;
    } catch {
        return null;
    }
}

/* No badge for nothing pending, or for a count lap could not give. */
export function badgeFor(count: number | null): Badge | undefined {
    if (count === null || count <= 0) return undefined;
    return { value: count, tooltip: `${count} ${count === 1 ? "file has" : "files have"} edits lap has not recorded yet` };
}

/* Asks lap for its status; calls back with stdout, or null on failure. */
export type AskStatus = (done: (stdout: string | null) => void) => void;

export class PendingCount {
    private timer: ReturnType<typeof setTimeout> | undefined;
    private running = false;
    private again = false;
    private disposed = false;

    constructor(
        private readonly ask: AskStatus,
        private readonly show: (badge: Badge | undefined) => void,
        private readonly settleMs = 1000,
    ) {}

    /* Something changed: count again once things settle. */
    schedule(): void {
        if (this.disposed) return;
        if (this.timer) clearTimeout(this.timer);
        this.timer = setTimeout(() => {
            this.timer = undefined;
            this.run();
        }, this.settleMs);
    }

    /* Count now (or right after the count already running). */
    run(): void {
        if (this.disposed) return;
        if (this.running) {
            this.again = true;
            return;
        }
        this.running = true;
        this.ask((stdout) => {
            this.running = false;
            if (this.disposed) return;
            this.show(badgeFor(stdout === null ? null : pendingCount(stdout)));
            if (this.again) {
                this.again = false;
                this.run();
            }
        });
    }

    dispose(): void {
        this.disposed = true;
        if (this.timer) clearTimeout(this.timer);
    }
}
