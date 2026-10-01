/* The messages between the extension host and a form's tab. */

import type { Answer, Request } from "ask";

import type { PreviewParts } from "./preview";

export type ToView =
    /** The form to show; sent once the tab is ready. */
    | { readonly type: "load"; readonly request: Request; readonly preview: PreviewParts }
    /** The form was answered or cancelled elsewhere: show it closed. */
    | { readonly type: "closed"; readonly status: Answer["status"] };

export type ToHost = { readonly type: "ready" } | { readonly type: "submit"; readonly answer: Answer } | { readonly type: "dismiss" };
