/* The messages between the History webview and the extension host. Types
 * only, so both sides compile against the same shapes. */

import type { HistoryFilter, HistoryPage } from "./history";

export type ToView =
    /* The page for the view's last query, sent again whenever the log grows
     * or the grouping changes. `hasRepo` false: no .lap here. */
    | { readonly type: "page"; readonly page: HistoryPage | null; readonly hasRepo: boolean; readonly active: string | null }
    | { readonly type: "collapseAll" };

export type ToHost =
    | { readonly type: "query"; readonly filter: HistoryFilter; readonly page: number }
    | { readonly type: "open"; readonly id: string };
