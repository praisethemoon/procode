/* One document refreshed from its own source, against the real kb binary and
 * a stand-in for the `vscode` module: a file that changed is re-indexed, one
 * that did not only moves its fetch date, a declined fetch files nothing, and
 * inline content says there is nowhere to go back to. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Kb } from "kb-js";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const KB = path.resolve(__dirname, "../../../../cli/kb-cli/bin/kb");
let answer: string | undefined = undefined;
const fake = {
    window: { showWarningMessage: async () => answer },
    workspace: { fs: { readFile: async (u: { fsPath: string }) => fs.readFileSync(u.fsPath) } },
    Uri: { file: (p: string) => ({ fsPath: p }) },
};

function load(): { refreshDocument: typeof import("../commands").refreshDocument } {
    const original = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : original(req, parent, isMain));
    try {
        return require("../commands") as { refreshDocument: typeof import("../commands").refreshDocument };
    } finally {
        Module._load = original;
    }
}

test("a document is refreshed from its own source", { skip: !fs.existsSync(KB) && "kb is not built" }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-refresh-"));
    const kb = new Kb({ bin: KB, cwd: dir });
    await kb.init();
    const { refreshDocument } = load();

    const file = path.join(dir, "notes.md");
    fs.writeFileSync(file, "# IOCP\n\nThe first version.\n");
    const filed = await kb.add(fs.readFileSync(file, "utf8"), {
        title: "Notes",
        collection: "win32",
        url: file,
        meta: { year: 2026 },
    });

    fs.writeFileSync(file, "# IOCP\n\nThe second version.\n");
    const changed = await refreshDocument(kb, filed.document);
    assert.equal(changed.outcome, "updated");
    const after = await kb.get(filed.document, { text: true });
    assert.match(after.text ?? "", /second version/);
    assert.equal(after.document.title, "Notes", "the title is kept");
    assert.equal(after.document.collection, "win32", "the collection is kept");
    assert.deepEqual(after.document.meta, { year: 2026 }, "the meta is kept");

    const same = await refreshDocument(kb, filed.document);
    assert.equal(same.outcome, "unchanged");

    fs.renameSync(file, file + ".moved");
    const gone = await refreshDocument(kb, filed.document);
    assert.equal(gone.outcome, "cannot");

    const inline = await kb.add("handed over\n", { title: "Inline", collection: "notes" });
    const none = await refreshDocument(kb, inline.document);
    assert.equal(none.outcome, "cannot");

    // A URL is fetched only when the reader agrees; declining files nothing.
    const page = await kb.add("<p>page</p>", { title: "Page", collection: "web", url: "https://example.invalid/page" });
    answer = undefined;
    const declined = await refreshDocument(kb, page.document);
    assert.equal(declined.outcome, "declined");
    assert.equal((await kb.get(page.document)).document.fetchedAt, page.fetchedAt);
});
