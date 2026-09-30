/* A document's Save As, against the real kb binary and a stand-in for the
 * `vscode` module: the stored text is written to the path the dialog returns,
 * the dialog starts on the document's name in the workspace folder, and a
 * cancelled dialog writes nothing. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Kb } from "kb-js";

import { TEST_ENV } from "./home";
import { cliBin, noCli } from "./cli-bin";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const KB = cliBin("kb");

type FakeUri = { fsPath: string; path: string };
const uri = (p: string): FakeUri => ({ fsPath: p, path: p });

let answer: FakeUri | undefined;
const dialogs: { defaultUri?: FakeUri; title?: string }[] = [];
const written: { to: string; bytes: Uint8Array }[] = [];
const warned: string[] = [];
const fake = {
    window: {
        showSaveDialog: async (options: { defaultUri?: FakeUri; title?: string }) => {
            dialogs.push(options);
            return answer;
        },
        showWarningMessage: async (m: string) => {
            warned.push(m);
            return undefined;
        },
        showErrorMessage: async (m: string) => {
            warned.push(m);
            return undefined;
        },
    },
    workspace: {
        workspaceFolders: undefined as { uri: FakeUri & { scheme: string } }[] | undefined,
        fs: {
            writeFile: async (to: FakeUri, bytes: Uint8Array) => {
                written.push({ to: to.fsPath, bytes });
                fs.writeFileSync(to.fsPath, bytes);
            },
        },
    },
    Uri: {
        file: (p: string) => uri(p),
        joinPath: (base: FakeUri, ...parts: string[]) => uri(path.join(base.fsPath, ...parts)),
    },
};

function load(): typeof import("../commands") {
    const original = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : original(req, parent, isMain));
    try {
        return require("../commands") as typeof import("../commands");
    } finally {
        Module._load = original;
    }
}

test("Save As writes the stored text where the reader picks, and nothing when cancelled", { skip: !KB && noCli("kb") }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-saveas-"));
    try {
        const project = path.join(dir, "project");
        fs.mkdirSync(project);
        const kb = new Kb({ bin: KB, cwd: project, env: TEST_ENV });
        await kb.init();
        const text = "# The scheduler\n\nWork is stolen from the back of the deque.\n";
        const filed = await kb.add(text, { title: "The scheduler", collection: "design", mime: "text/markdown" });
        const { saveDocumentAs } = load();
        fake.workspace.workspaceFolders = [{ uri: { scheme: "file", ...uri(project) } }];

        // Cancelled: nothing written.
        answer = undefined;
        assert.equal(await saveDocumentAs(kb, filed.document), null);
        assert.equal(written.length, 0);
        assert.equal(dialogs[0].defaultUri?.fsPath, path.join(project, "The scheduler.md"), "starts on its name in the workspace");

        // Picked: the stored text, byte for byte.
        const target = path.join(dir, "out.md");
        answer = uri(target);
        assert.equal(await saveDocumentAs(kb, filed.document), target);
        assert.equal(fs.readFileSync(target, "utf8"), text);
        assert.equal((await kb.get(filed.document, { text: true })).text, text, "the stored document is unchanged");

        // A document that does not exist is reported, and nothing is written.
        assert.equal(await saveDocumentAs(kb, "D-999999"), null);
        assert.equal(written.length, 1);
        assert.equal(warned.length, 1);
    } finally {
        fake.workspace.workspaceFolders = undefined;
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
