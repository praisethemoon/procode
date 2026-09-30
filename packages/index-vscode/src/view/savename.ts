/* What a document is called outside the store: the name its toolbar shows,
 * and the file name Save As suggests. Pure: no vscode, no kb.
 *
 * A DOCUMENT FILED FROM A FILE KEEPS THAT FILE'S NAME, extension included,
 * because that is what the reader knows it by. Anything else (a web page, text
 * an agent handed over) is named after its title, with an extension from its
 * type so the saved file opens as what it is. */

import { localPath } from "./locator";

export interface Nameable {
    readonly title: string;
    readonly path: string;
    readonly locator: string;
    readonly mime: string;
}

const EXTENSION_OF_MIME: Readonly<Record<string, string>> = {
    "text/markdown": ".md",
    "text/html": ".html",
    "text/plain": ".txt",
    "application/json": ".json",
    "text/csv": ".csv",
    "application/xml": ".xml",
    "text/xml": ".xml",
    "application/pdf": ".txt",
};

function basename(p: string): string {
    return p.split(/[\\/]/).filter((s) => s.length > 0).pop() ?? "";
}

/* The file the document came from, when it came from one: its path inside a
 * filed folder, else the locator's own file. */
function fileName(d: Nameable): string {
    if (d.path.trim() !== "") return basename(d.path);
    const local = localPath(d.locator);
    return local === null ? "" : basename(local);
}

/* The toolbar's name: the source file's name, else the title. */
export function documentName(d: Nameable): string {
    return fileName(d) || d.title.trim();
}

/* Save As's suggested file name: the source file's name as it is, else the
 * title made safe for a file system, with an extension from the type. A PDF
 * saves as .txt, since the store holds its extracted text, not its bytes. */
export function saveName(d: Nameable): string {
    const own = fileName(d);
    if (own !== "" && !d.mime.startsWith("application/pdf")) return own;
    const stem =
        (own !== "" ? own.replace(/\.[^.]+$/, "") : d.title)
            .replace(/[\\/:*?"<>|\u0000-\u001f]+/g, " ")
            .replace(/\s+/g, " ")
            .trim()
            .slice(0, 120) || "document";
    const ext = EXTENSION_OF_MIME[d.mime.split(";")[0].trim().toLowerCase()] ?? ".txt";
    return stem.toLowerCase().endsWith(ext) ? stem : stem + ext;
}
