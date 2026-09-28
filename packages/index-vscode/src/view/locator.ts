/* index-ui.md §3.1: where a document's source link goes. Pure, so the
 * webview, the host and the tests run the same code.
 *
 * LOCAL SOURCES OPEN IN VS CODE. A file filed with `kb add` has its own path
 * as its locator; a folder's files share the folder's path as theirs and are
 * each at their `path` under it (index-api §2.1). Either is written as a bare
 * absolute path, or as a `file://` URL by whatever filed it; both are read, so
 * a store needs no re-filing. The link names the file itself, so what the
 * header shows is what opens.
 *
 * WEB SOURCES OPEN EXTERNALLY, as before: http, https and mailto only, behind
 * the host's confirmation. Anything else is refused. */

/* A bare absolute path: POSIX, a Windows drive, or a UNC share. */
function isAbsolutePath(s: string): boolean {
    return s.startsWith("/") || /^[A-Za-z]:[\\/]/.test(s) || s.startsWith("\\\\");
}

/* The local path a locator names, or null when it is not local. */
export function localPath(locator: string): string | null {
    const s = locator.trim();
    if (/^file:/i.test(s)) {
        try {
            const u = new URL(s);
            const p = decodeURIComponent(u.pathname);
            /* file:///C:/x reads as /C:/x; a host is a UNC share */
            if (u.host !== "") return `//${u.host}${p}`;
            return /^\/[A-Za-z]:\//.test(p) ? p.slice(1) : p;
        } catch {
            return null;
        }
    }
    return isAbsolutePath(s) ? s : null;
}

/* What a document's header shows and its link opens: for a local document,
 * its own file (a folder's path joined with the document's path under it);
 * otherwise the locator as filed. */
export function documentLocation(locator: string, path: string): string {
    const base = localPath(locator);
    const rel = path.replace(/^[\\/]+/, "");
    if (base === null || rel === "") return base ?? locator;
    const windows = base.includes("\\") && !base.includes("/");
    const sep = windows ? "\\" : "/";
    return base.replace(/[\\/]+$/, "") + sep + (windows ? rel.replace(/\//g, "\\") : rel.replace(/\\/g, "/"));
}

export type LinkTarget =
    | { readonly kind: "file"; readonly path: string }
    | { readonly kind: "missing"; readonly path: string }
    | { readonly kind: "web"; readonly url: string }
    | { readonly kind: "refused"; readonly href: string };

const WEB = /^(https?|mailto):/i;

/* What a source link does: open a local file (or say it is gone), open a web
 * address externally, or nothing. */
export function linkTarget(href: string, exists: (path: string) => boolean): LinkTarget {
    const s = href.trim();
    const local = localPath(s);
    if (local !== null) return exists(local) ? { kind: "file", path: local } : { kind: "missing", path: local };
    if (WEB.test(s)) return { kind: "web", url: s };
    return { kind: "refused", href: s };
}
