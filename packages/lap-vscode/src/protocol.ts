/* The messages between the History webview and the extension host. Types
 * only, so both sides compile against the same shapes. */

import type { BranchView } from "./branches";
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
          /* The branches this folder started (lap branch list), each with its
           * own sessions; empty where there are none or lap cannot say. */
          readonly branches: readonly BranchView[];
          /* why the history cannot be shown (a chunk missing from its
           * middle), in lap's words; then page is empty */
          readonly problem: string | null;
          /* why lap itself failed (not installed, too old for this
           * history, refusing it), in its words; the log is still shown */
          readonly lapError: string | null;
      }
    | { readonly type: "collapseAll" };

export type ToHost =
    | { readonly type: "query"; readonly filter: HistoryFilter; readonly page: number }
    | { readonly type: "open"; readonly id: string }
    /* A reference in a commit's text (`#<hex>` or `L<n>`) was followed. */
    | { readonly type: "reveal"; readonly ref: string }
    /* An adopted commit's original, by hash: shown as lap shows it. */
    | { readonly type: "original"; readonly hash: string }
    /* The fix for a missing branch: point it at its folder, or drop it. */
    | { readonly type: "branchFix"; readonly action: "move" | "forget"; readonly name: string };
