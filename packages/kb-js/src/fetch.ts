/* Fetching a web page to file it. kb itself never touches the network; the
 * Knowledge view's Add URL and the kb MCP server's kb_add with urls both fetch
 * through this, so a page is filed the same way from either.
 *
 * THE BYTES ARE FILED AS THEY ARRIVED. A page fetched and then rewritten here
 * — tags stripped, entities decoded, whitespace collapsed — would be a
 * document whose content hash covers this code's opinion of the page rather
 * than the page. Knowledge renders HTML as prose at READ time, from the stored
 * bytes, which is where an opinion can be changed without invalidating
 * anything.
 *
 * A REDIRECT IS FOLLOWED AND A NON-2xx IS NOT A DOCUMENT. index-api §11 has
 * `fetch_failed` carrying "status and locator" for exactly this, and filing a
 * 404 page under a title somebody chose is how a store comes to contain a
 * confident answer that is an error page. */

export type FetchedPage =
    | { readonly notModified: false; readonly text: string; readonly mime: string; readonly title: string; readonly etag: string | null }
    /* A conditional request answered 304: the page is the one already filed. */
    | { readonly notModified: true };

export async function fetchPage(url: string, ifNoneMatch?: string | null): Promise<FetchedPage> {
    const headers: Record<string, string> = { accept: "text/html,text/markdown,text/plain;q=0.9,*/*;q=0.5" };
    if (ifNoneMatch) {
        headers["if-none-match"] = ifNoneMatch;
    }
    let response: Response;
    try {
        response = await fetch(url, { redirect: "follow", headers });
    } catch (e) {
        /* fetch's own message is "fetch failed"; the reason is in its cause. */
        const cause = (e as { cause?: { message?: string; code?: string } }).cause;
        const why = cause?.code ?? cause?.message ?? (e as Error).message;
        throw new Error(`Could not reach ${new URL(url).host}: ${why}.`);
    }
    if (response.status === 304 && ifNoneMatch) {
        return { notModified: true };
    }
    if (!response.ok) {
        throw new Error(`${url} answered ${response.status} ${response.statusText}; nothing was filed.`);
    }
    const contentType = response.headers.get("content-type") ?? "";
    const mime = contentType.split(";")[0].trim().toLowerCase() || "text/plain";
    const text = await response.text();
    return { notModified: false, text, mime, title: titleOf(text, mime, url), etag: response.headers.get("etag") };
}

/* A title read out of the page when it has one, because "Untitled" in a
 * provenance block is a document nobody will find again; else the host and
 * the last part of the path. */
export function titleOf(text: string, mime: string, url: string): string {
    if (mime === "text/html") {
        const m = text.match(/<title[^>]*>([\s\S]{0,300}?)<\/title>/i);
        if (m !== null) {
            const title = m[1].replace(/\s+/g, " ").trim();
            if (title.length > 0) {
                return title;
            }
        }
    }
    if (mime === "text/markdown") {
        const m = text.match(/^#\s+(.+)$/m);
        if (m !== null) {
            return m[1].trim();
        }
    }
    try {
        const parsed = new URL(url);
        const last = parsed.pathname.split("/").filter((p) => p.length > 0).pop();
        return last === undefined ? parsed.host : `${parsed.host} ${last}`;
    } catch {
        return url;
    }
}
