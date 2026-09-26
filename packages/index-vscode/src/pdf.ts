/* A paper filed from its PDF (index-api §12.3: "ingesting pre-extracted text
 * avoids" putting PDF extraction in the CLI).
 *
 * THE TEXT IS EXTRACTED HERE, BY pdf.js, AND FILED AS MARKDOWN. One `##`
 * section per page, under a `#` title, so the heading chunker splits on pages
 * and a search hit says which page it came from. The PDF itself is not
 * stored: kb holds text, and the locator is the PDF's path, so Refresh reads
 * the PDF again and re-extracts.
 *
 * pdf.js is ES modules and loads its worker at runtime, so it is not bundled
 * into the extension: the build copies it beside the bundle (out/pdfjs/) and
 * it is imported from there — or from node_modules when running unbundled,
 * as the tests do.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import { pathToFileURL } from "node:url";

/* One positioned run of text, as pdf.js reports it. */
export interface TextRun {
    readonly str: string;
    /* The run's baseline, in PDF units from the bottom of the page. */
    readonly y: number;
    readonly height: number;
    readonly hasEOL: boolean;
}

export interface PdfText {
    readonly markdown: string;
    readonly title: string;
    readonly pages: number;
    readonly meta: Readonly<Record<string, unknown>>;
}

/* A line that would read as Markdown structure — a heading, a list item, a
 * quote, a rule — is escaped: it is the paper's prose, not our formatting. */
function escapeLine(line: string): string {
    return line.replace(/^(\s*)([#>*+-]|\d+[.)])(\s)/, "$1\\$2$3").replace(/^(\s*)(-{3,}|_{3,}|\*{3,})\s*$/, "$1\\$2");
}

/* Runs of one page back into lines and paragraphs. A new line where pdf.js
 * says one ended or where the baseline moves; a blank line (a paragraph
 * break) where the gap between baselines is clearly wider (a third more)
 * than the page's usual line spacing. */
export function pageText(runs: readonly TextRun[]): string {
    const lines: { y: number; text: string }[] = [];
    let current = "";
    let y: number | null = null;
    const flush = () => {
        const t = current.replace(/\s+/g, " ").trim();
        if (t !== "" && y !== null) lines.push({ y, text: t });
        current = "";
    };
    for (const r of runs) {
        if (y !== null && Math.abs(r.y - y) > Math.max(1, r.height * 0.5) && current !== "") {
            flush();
        }
        if (current === "") y = r.y;
        current += r.str;
        if (r.hasEOL) flush();
    }
    flush();
    if (lines.length === 0) return "";
    const gaps = lines.slice(1).map((l, i) => Math.abs(lines[i].y - l.y)).filter((g) => g > 0);
    /* The lower median: with few lines, the upper one can be the paragraph
     * gap itself, which would then read as the page's normal spacing. */
    const usual = gaps.length > 0 ? [...gaps].sort((a, b) => a - b)[Math.floor((gaps.length - 1) / 2)] : 0;
    let out = escapeLine(lines[0].text);
    for (let i = 1; i < lines.length; i++) {
        const gap = Math.abs(lines[i - 1].y - lines[i].y);
        out += usual > 0 && gap > usual * 1.3 ? "\n\n" : "\n";
        out += escapeLine(lines[i].text);
    }
    return out;
}

/* The whole document as Markdown, from its pages' runs and its metadata. */
export function pdfMarkdown(pages: readonly (readonly TextRun[])[], info: { title?: string; author?: string }, fallbackTitle: string): PdfText {
    const title = (info.title ?? "").trim() || fallbackTitle;
    const parts = [`# ${title.replace(/\n/g, " ")}`];
    if (info.author?.trim()) parts.push(`*${info.author.trim()}*`);
    pages.forEach((runs, i) => {
        const text = pageText(runs);
        parts.push(`## Page ${i + 1}` + (text ? `\n\n${text}` : "\n\n*(no text on this page)*"));
    });
    const meta: Record<string, unknown> = { pdf: { pages: pages.length } };
    if (info.author?.trim()) meta["authors"] = [info.author.trim()];
    return { markdown: parts.join("\n\n") + "\n", title, pages: pages.length, meta };
}

/* pdf.js's module: the copy beside the bundle, or the package when unbundled. */
function pdfjsUrl(): string {
    const beside = path.join(__dirname, "pdfjs", "pdf.mjs");
    const file = fs.existsSync(beside) ? beside : require.resolve("pdfjs-dist/legacy/build/pdf.mjs");
    return pathToFileURL(file).href;
}

/* A real dynamic import: the bundler and the TypeScript compiler would both
 * rewrite a literal `import()` into `require()`, which cannot load ES modules. */
const importModule = new Function("url", "return import(url)") as (url: string) => Promise<unknown>;

interface PdfJs {
    getDocument(src: object): { promise: Promise<PdfDoc>; destroy(): Promise<void> };
    VerbosityLevel: { ERRORS: number };
}
interface PdfDoc {
    numPages: number;
    getMetadata(): Promise<{ info: Record<string, unknown> }>;
    getPage(n: number): Promise<{ getTextContent(): Promise<{ items: unknown[] }> }>;
}

export async function extractPdf(bytes: Uint8Array, fallbackTitle: string): Promise<PdfText> {
    const pdfjs = (await importModule(pdfjsUrl())) as PdfJs;
    const task = pdfjs.getDocument({
        data: new Uint8Array(bytes),
        isEvalSupported: false,
        useSystemFonts: false,
        verbosity: pdfjs.VerbosityLevel.ERRORS,
    });
    let doc: PdfDoc;
    try {
        doc = await task.promise;
    } catch (e) {
        await task.destroy();
        throw new Error(`not a PDF this can read: ${(e as Error).message}`);
    }
    try {
        const info: Record<string, unknown> = (await doc.getMetadata().catch(() => ({ info: {} }))).info;
        const pages: TextRun[][] = [];
        for (let n = 1; n <= doc.numPages; n++) {
            const content = await (await doc.getPage(n)).getTextContent();
            pages.push(
                content.items
                    .filter((it): it is { str: string; transform: number[]; height: number; hasEOL: boolean } => typeof (it as { str?: unknown }).str === "string")
                    .map((it) => ({ str: it.str, y: it.transform[5], height: it.height, hasEOL: it.hasEOL })),
            );
        }
        const text = (v: unknown) => (typeof v === "string" ? v : undefined);
        return pdfMarkdown(pages, { title: text(info["Title"]), author: text(info["Author"]) }, fallbackTitle);
    } finally {
        await task.destroy();
    }
}

export function isPdf(file: string): boolean {
    return /\.pdf$/i.test(file);
}
