/* A folder added from the view and a folder's document refreshed, against the
 * real kb binary and a stand-in for the `vscode` module: the folder picked is
 * filed whole with progress shown, filing it again forgets what is gone and
 * says so, and a document of it is refreshed by walking the folder without
 * forgetting its neighbours. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Kb } from "kb-js";

import { TEST_ENV } from "./home";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const KB = path.resolve(__dirname, "../../../../cli/kb-cli/bin/kb");

/* What the dialogs answer, and what the reader was shown. */
let folder: string | undefined = undefined;
let collection: string | undefined = undefined;
const opened: Record<string, unknown>[] = [];
const progress: string[] = [];
const told: string[] = [];
const warned: string[] = [];
const fake = {
    window: {
        showOpenDialog: async (options: Record<string, unknown>) => {
            opened.push(options);
            return folder === undefined ? undefined : [{ fsPath: folder, path: folder }];
        },
        showQuickPick: async () => collection,
        showInputBox: async () => collection,
        withProgress: async (options: { title: string }, task: () => Promise<unknown>) => {
            progress.push(options.title);
            return task();
        },
        showInformationMessage: async (message: string) => {
            told.push(message);
            return undefined;
        },
        showWarningMessage: async (message: string) => {
            warned.push(message);
            return undefined;
        },
        showErrorMessage: async (message: string) => {
            warned.push(message);
            return undefined;
        },
    },
    ProgressLocation: { Notification: 15 },
    workspace: { fs: { readFile: async (u: { fsPath: string }) => fs.readFileSync(u.fsPath) } },
    Uri: { file: (p: string) => ({ fsPath: p }) },
};

type Commands = typeof import("../commands");

function load(): Pick<Commands, "addFolder" | "refreshDocument"> {
    const original = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : original(req, parent, isMain));
    try {
        return require("../commands") as Commands;
    } finally {
        Module._load = original;
    }
}

test("a folder is added whole, and added again forgets what is gone", { skip: !fs.existsSync(KB) && "kb is not built" }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-folder-"));
    try {
        const project = path.join(dir, "project");
        fs.mkdirSync(project);
        const kb = new Kb({ bin: KB, cwd: project, env: TEST_ENV });
        await kb.init();
        const { addFolder, refreshDocument } = load();

        const tree = path.join(dir, "notes");
        fs.mkdirSync(path.join(tree, "sub"), { recursive: true });
        fs.writeFileSync(path.join(tree, "iocp.md"), "# IOCP\n\nzzfirst\n");
        fs.writeFileSync(path.join(tree, "sub", "ring.md"), "# Ring\n\nzzring\n");
        fs.writeFileSync(path.join(tree, "sub", "gone.md"), "# Gone\n\nzzgone\n");

        // Nothing picked: nothing filed and nothing said.
        let announced = 0;
        folder = undefined;
        await addFolder(kb, () => announced++);
        assert.equal(announced, 0);
        assert.deepEqual(told, []);
        assert.equal(opened[0]["canSelectFolders"], true);
        assert.equal(opened[0]["canSelectFiles"], false);

        folder = tree;
        collection = "research";
        await addFolder(kb, () => announced++);
        assert.equal(announced, 1, "the views are told the store moved");
        assert.deepEqual(progress, ["Filing notes into research…"]);
        assert.match(told[0], /^Filed notes into research as S-\d+: 3 files, 3 added, 0 updated, 0 unchanged, 0 forgotten, 0 skipped\.$/);
        const source = (await kb.sources({ kind: "dir" }))[0];
        assert.equal(source.collection, "research");
        const docs = (await kb.source(source.id)).documents;
        const ring = docs.find((d) => d.path === "sub/ring.md");
        const gone = docs.find((d) => d.path === "sub/gone.md");
        assert.ok(ring !== undefined && gone !== undefined);

        /* A document of the folder, refreshed: the folder is walked again,
         * and a neighbour gone from it is not forgotten on the way. */
        fs.rmSync(path.join(tree, "sub", "gone.md"));
        assert.equal((await refreshDocument(kb, ring.id)).outcome, "unchanged");
        fs.writeFileSync(path.join(tree, "sub", "ring.md"), "# Ring\n\nzzchanged\n");
        const changed = await refreshDocument(kb, ring.id);
        assert.equal(changed.outcome, "updated");
        assert.match((await kb.get(ring.id, { text: true })).text ?? "", /zzchanged/);
        assert.equal((await kb.search("zzgone")).count, 1, "refreshing one document forgot another");
        const cannot = await refreshDocument(kb, gone.id);
        assert.equal(cannot.outcome, "cannot");

        // Added again from the view: the reader's own action, so what is gone is forgotten.
        await addFolder(kb, () => announced++, "research");
        assert.match(told[1], /: 2 files, 0 added, 0 updated, 2 unchanged, 1 forgotten, 0 skipped\.$/);
        assert.equal((await kb.search("zzgone")).count, 0);

        // A folder kb will not file is reported, and nothing is announced.
        const before = announced;
        folder = path.join(tree, "iocp.md");
        await addFolder(kb, () => announced++, "research");
        assert.equal(announced, before);
        assert.equal(warned.length, 1);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
