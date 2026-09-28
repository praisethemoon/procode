/* The messages between the pages view's webview and the extension host. */

import type { Page } from "eggzibit";

/* A page as the list shows it: its metadata, without the page. */
export type PageRow = Omit<Page, "bytes">;

export type ToView = {
    readonly type: "pages";
    /* newest update first */
    readonly pages: readonly PageRow[];
    /* false outside a folder: there is nowhere for pages to be */
    readonly hasFolder: boolean;
};

export type ToHost =
    | { readonly type: "ready" }
    | { readonly type: "open"; readonly id: string }
    | { readonly type: "source"; readonly id: string };
