/* The messages between the History webview and the extension host. Types
 * only, so both sides compile against the same shapes. */

import type { HistoryFilter, HistoryPage } from "./history";

export type ToView =
    /* The page for the view's last query, sent again whenever the log grows
     * or the grouping changes. `hasRepo` false: no .lap here. `reveal`: the
     * page was chosen to show that commit, under `filter` — the view's filter
     * when it showed the commit, or one that does. */
    | {
          readonly type: "page";
          readonly page: HistoryPage | null;
          readonly hasRepo: boolean;
          readonly active: string | null;
          readonly reveal: { readonly id: string; readonly filter: HistoryFilter } | null;
      }
    | { readonly type: "collapseAll" };

export type ToHost =
    | { readonly type: "query"; readonly filter: HistoryFilter; readonly page: number }
    | { readonly type: "open"; readonly id: string }
    /* A reference in a commit's text (`#<hex>` or `L<n>`) was followed. */
    | { readonly type: "reveal"; readonly ref: string };
