/* Filing now and embedding later (index-api §2), from the view: a filing that
 * left chunks to embed says how many and offers to finish them, and finishing
 * runs `kb embed` with progress shown and the views told. Against a stand-in
 * for the `vscode` module, and for the refusal against the real kb, whose
 * throwaway home holds no model. */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";

import { Kb, KbDirAdded, KbEmbedded } from "kb-js";

import { TEST_ENV } from "./home";
import { cliBin, noCli } from "./cli-bin";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const Module = require("node:module") as { _load: (req: string, parent: unknown, isMain: boolean) => unknown };

const KB = cliBin("kb");

/* What the dialogs answer, and what the reader was shown. */
let answer: string | undefined = undefined;
const progress: string[] = [];
const told: { message: string; actions: string[] }[] = [];
const warned: string[] = [];
const fake = {
    window: {
        showOpenDialog: async () => [{ fsPath: "/work/notes", path: "/work/notes" }],
        showQuickPick: async () => "research",
        showInputBox: async () => "research",
        withProgress: async (options: { title: string }, task: () => Promise<unknown>) => {
            progress.push(options.title);
            return task();
        },
        showInformationMessage: async (message: string, ...actions: string[]) => {
            told.push({ message, actions });
            return actions.length > 0 ? answer : undefined;
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

function load(): Commands {
    const original = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? fake : original(req, parent, isMain));
    try {
        return require("../commands") as Commands;
    } finally {
        Module._load = original;
    }
}

function reset(): void {
    answer = undefined;
    progress.length = 0;
    told.length = 0;
    warned.length = 0;
}

/* A folder filing that left `pending` chunks, and an embed pass that finishes
 * them: the two answers the flow reads, without a model on this machine. */
function stub(pending: number, embeds: { n: number }): Kb {
    const filed: KbDirAdded = {
        source: "S-4",
        root: "/work/notes",
        collection: "research",
        files: 3,
        added: 3,
        updated: 0,
        unchanged: 0,
        forgotten: [],
        missing: [],
        skipped: { ignored: 0, hidden: 0, vendored: 0, generated: 0, binary: 0, large: 0, unreadable: 0, otherTypes: 0 },
        embedded: 12,
        pending,
    };
    const done: KbEmbedded = { embedded: pending, kept: 12, skipped: 0, pending: 0 };
    return {
        collections: async () => [],
        addDir: async () => filed,
        embed: async () => {
            embeds.n++;
            return done;
        },
    } as unknown as Kb;
}

/* The offer is answered after the command returns, as a notification is. */
async function settle(): Promise<void> {
    for (let i = 0; i < 5; i++) {
        await new Promise((r) => setImmediate(r));
    }
}

test("a filing that left chunks to embed says how many and offers to finish them", async () => {
    reset();
    const { addFolder, FINISH_EMBEDDING } = load();
    const embeds = { n: 0 };
    let announced = 0;

    answer = FINISH_EMBEDDING;
    await addFolder(stub(812, embeds), () => announced++, "research");
    await settle();
    assert.match(told[0].message, /^Filed notes into research as S-4: .* 812 chunks left to embed: searchable by keyword now/);
    assert.deepEqual(told[0].actions, [FINISH_EMBEDDING]);
    assert.equal(embeds.n, 1, "choosing Finish embedding runs kb embed");
    assert.deepEqual(progress, ["Filing notes into research…", "Embedding the chunks left to embed…"]);
    assert.equal(told[1].message, "Embedded 812 chunks.");
    assert.equal(announced, 2, "the views are told after the filing and again after the embedding");

    // Dismissed: nothing is embedded.
    reset();
    await addFolder(stub(3, embeds), () => announced++, "research");
    await settle();
    assert.equal(embeds.n, 1);
    assert.match(told[0].message, /3 chunks left to embed/);

    // Nothing pending: the plain message, and no offer.
    reset();
    answer = FINISH_EMBEDDING;
    await addFolder(stub(0, embeds), () => announced++, "research");
    await settle();
    assert.deepEqual(told[0].actions, []);
    assert.doesNotMatch(told[0].message, /left to embed/);
    assert.equal(embeds.n, 1);
});

test("the note is empty when nothing is pending and counts one chunk as one", () => {
    const { pendingNote } = load();
    assert.equal(pendingNote(0), "");
    assert.match(pendingNote(1), /^ 1 chunk left to embed/);
    assert.match(pendingNote(2), /^ 2 chunks left to embed/);
});

test("finishing with no model is kb's refusal, shown with its code", { skip: !KB && noCli("kb") }, async () => {
    reset();
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "kb-embed-"));
    try {
        const kb = new Kb({ bin: KB, cwd: dir, env: TEST_ENV });
        await kb.init();
        await kb.add("zzembed\n", { title: "t", collection: "c", embedBudget: 0 });
        const { finishEmbedding } = load();
        let announced = 0;
        await finishEmbedding(kb, () => announced++);
        assert.deepEqual(progress, ["Embedding the chunks left to embed…"]);
        assert.equal(announced, 0, "nothing was embedded, so nothing moved");
        assert.equal(warned.length, 1);
        assert.match(warned[0], /\(model_missing\)$/);
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
});
