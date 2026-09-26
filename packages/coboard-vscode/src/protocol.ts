/* The messages between a board tab and the extension host. Types only, so
 * both sides compile against the same shapes. */

import type { LapCommit, LapSession, Summary, View } from "coboard";

export interface Choices {
    /* Every epic and milestone, for the "move to" pickers. */
    readonly epics: readonly Summary[];
    readonly milestones: readonly Summary[];
}

export interface Sessions {
    readonly ok: boolean;
    readonly sessions: readonly LapSession[];
    readonly error?: string;
}

export type ToView =
    | { readonly type: "data"; readonly view: View | null; readonly id: string; readonly choices: Choices }
    | { readonly type: "sessions"; readonly ticket: string; readonly sessions: Sessions }
    | { readonly type: "commits"; readonly session: string; readonly commits: readonly LapCommit[]; readonly error?: string }
    | { readonly type: "error"; readonly message: string };

export type Fields = {
    title?: string;
    description?: string;
    status?: string;
    size?: string | null;
    priority?: string;
    assignee?: string | null;
    labels?: string[];
};

export type ToHost =
    | { readonly type: "ready" }
    | { readonly type: "open"; readonly id: string }
    | { readonly type: "update"; readonly id: string; readonly fields: Fields }
    | { readonly type: "create"; readonly kind: "milestone" | "ticket"; readonly title: string; readonly epic?: string; readonly milestone?: string }
    | { readonly type: "move"; readonly id: string; readonly epic?: string; readonly milestone?: string | null }
    | { readonly type: "comment"; readonly ticket: string; readonly body: string }
    | { readonly type: "delete"; readonly id: string }
    | { readonly type: "startSession"; readonly ticket: string }
    | { readonly type: "commits"; readonly session: string }
    | { readonly type: "showEdit"; readonly commit: string; readonly sessionMsg?: string };

/* The sidebar: the whole board as summaries, and what a row can ask for. */
export type SidebarToView =
    | { readonly type: "items"; readonly items: readonly Summary[]; readonly hasFolder: boolean }
    | { readonly type: "collapseAll" };

export type SidebarToHost =
    | { readonly type: "ready" }
    | { readonly type: "open"; readonly id: string }
    | { readonly type: "command"; readonly command: "coboard.newEpic" | "coboard.newMilestone" | "coboard.newTicket"; readonly id?: string };
