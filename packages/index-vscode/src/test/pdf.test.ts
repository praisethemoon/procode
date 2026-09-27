/* PDFs: the layout of extracted text into Markdown, and a hand-built PDF
 * filed through pdf.js and the real kb binary, searched by page. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Kb } from "kb-js";

import { TEST_ENV } from "./home";

import { extractPdf, isPdf, pageText, pdfMarkdown } from "../pdf";
import { cliBin, noCli } from "./cli-bin";

const KB = cliBin("kb");

/* A minimal valid PDF: pages of text lines, and an Info dictionary. */
function makePdf(pages: string[][], info: { title: string; author: string }): Buffer {
    const objs: (string | null)[] = [];
    const add = (s: string | null) => objs.push(s);
    const catalog = add(null);
    const pagesObj = add(null);
    const font = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const kids: number[] = [];
    for (const lines of pages) {
        let y = 750;
        const ops = ["BT", "/F1 12 Tf"];
        for (const l of lines) {
            if (l === "") {
                y -= 24; // a paragraph gap
                continue;
            }
            ops.push(`1 0 0 1 72 ${y} Tm`, `(${l.replace(/[()\\]/g, (c) => "\\" + c)}) Tj`);
            y -= 16;
        }
        ops.push("ET");
        const stream = ops.join("\n");
        const content = add(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
        kids.push(add(`<< /Type /Page /Parent ${pagesObj} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 ${font} 0 R >> >> /Contents ${content} 0 R >>`));
    }
    objs[catalog - 1] = `<< /Type /Catalog /Pages ${pagesObj} 0 R >>`;
    objs[pagesObj - 1] = `<< /Type /Pages /Kids [${kids.map((k) => `${k} 0 R`).join(" ")}] /Count ${kids.length} >>`;
    const infoObj = add(`<< /Title (${info.title}) /Author (${info.author}) >>`);
    let out = "%PDF-1.4\n";
    const offsets: number[] = [];
    objs.forEach((o, i) => {
        offsets.push(out.length);
        out += `${i + 1} 0 obj\n${o}\nendobj\n`;
    });
    const xref = out.length;
    out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n` + offsets.map((o) => `${String(o).padStart(10, "0")} 00000 n \n`).join("");
    out += `trailer\n<< /Size ${objs.length + 1} /Root ${catalog} 0 R /Info ${infoObj} 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
    return Buffer.from(out, "latin1");
}

test("runs become lines, wide gaps become paragraphs, and Markdown in the prose is escaped", () => {
    const run = (str: string, y: number, hasEOL = false) => ({ str, y, height: 12, hasEOL });
    const text = pageText([
        run("Completion ", 750),
        run("ports", 750, true),
        run("queue finished I/O.", 734),
        run("# not a heading", 700),
        run("1. not a list", 684),
    ]);
    assert.equal(text, "Completion ports\nqueue finished I/O.\n\n\\# not a heading\n\\1. not a list");
    assert.equal(pageText([]), "");
});

test("the document is a title, its author, and a section per page", () => {
    const md = pdfMarkdown([[{ str: "one", y: 700, height: 12, hasEOL: false }], []], { title: "Async IO", author: "Ana" }, "fallback");
    assert.equal(md.title, "Async IO");
    assert.equal(md.pages, 2);
    assert.match(md.markdown, /^# Async IO\n\n\*Ana\*\n\n## Page 1\n\none\n\n## Page 2\n\n\*\(no text on this page\)\*\n$/);
    assert.deepEqual(md.meta, { pdf: { pages: 2 }, authors: ["Ana"] });
    assert.equal(pdfMarkdown([], {}, "paper").title, "paper", "no title in the PDF: the file's name");
    assert.equal(isPdf("/a/Paper.PDF"), true);
    assert.equal(isPdf("/a/paper.md"), false);
});

test("a PDF is read by pdf.js and filed so its pages are searchable", { skip: !KB && noCli("kb") }, async () => {
    const pdf = makePdf(
        [
            // Pages of real length: kb merges tiny sibling sections, and a page of one
            // line would share a chunk (and a heading) with the next.
            ["Completion ports", "A completion port queues finished zzoverlapped I/O.",
             "Worker threads wait on the port and take one completion each,",
             "so the number of threads running at once stays near the core count,",
             "and a burst of finished requests wakes no more threads than can work."],
            ["Rings", "io_uring shares a zzsubmission ring with the kernel.", "", "A second paragraph.",
             "The application writes entries into the submission ring,",
             "the kernel writes results into the completion ring, and a busy",
             "program submits and reaps many requests without a call for each one."],
        ],
        { title: "Async IO notes", author: "Ana" },
    );
    const paper = await extractPdf(pdf, "fallback");
    assert.equal(paper.title, "Async IO notes");
    assert.equal(paper.pages, 2);
    assert.match(paper.markdown, /## Page 2\n\nRings\nio_uring shares a zzsubmission ring with the kernel\.\n\nA second paragraph\./);

    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-pdf-"));
    const kb = new Kb({ bin: KB, cwd: dir, env: TEST_ENV });
    await kb.init();
    const file = path.join(dir, "notes.pdf");
    fs.writeFileSync(file, pdf);
    await kb.add(paper.markdown, { title: paper.title, collection: "papers", url: file, mime: "text/markdown", meta: paper.meta });
    const hit = (await kb.search("zzsubmission")).hits[0];
    assert.equal(hit?.heading, "Page 2", "a hit says which page it is on");
    assert.equal((await kb.ls())[0].locator, file, "the locator is the PDF's path");

    await assert.rejects(extractPdf(Buffer.from("not a pdf"), "x"), /not a PDF this can read/);
});
