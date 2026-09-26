/* What reaches a Knowledge view when there is no store, when kb cannot be
 * started, and when a store refuses something in it: three different
 * answers, because each asks the reader for something different. The host is
 * driven through `handleRequest` with a stand-in for the `vscode` module and a
 * real kb. */

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

type Host = typeof import("../host");

function loadHost(): Host {
    const load = Module._load;
    Module._load = (req, parent, isMain) => (req === "vscode" ? {} : load(req, parent, isMain));
    try {
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        return require("../host") as Host;
    } finally {
        Module._load = load;
    }
}

/* One call through the host, and the one message it posts back. */
async function ask(dir: string, bin: string, op: string, input: unknown = {}): Promise<Record<string, unknown>> {
    const host = loadHost();
    const posted: Record<string, unknown>[] = [];
    const client = new Kb({ bin, cwd: dir, env: TEST_ENV });
    const ctx = {
        extensionUri: {},
        client: () => client,
        settings: () => ({ cliPath: bin }),
        announce() {},
        open() {},
        scope() {},
        retitle() {},
    } as unknown as Parameters<Host["handleRequest"]>[0];
    host.handleRequest(ctx, { webview: { postMessage: async (m: Record<string, unknown>) => posted.push(m) } } as never, {
        kind: "call",
        id: 1,
        op,
        input,
    });
    for (let i = 0; i < 200 && posted.length === 0; i++) await new Promise((r) => setTimeout(r, 10));
    assert.equal(posted.length, 1);
    return posted[0];
}

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "knowledge-states-"));

test("a folder with no store is marked as having none, not shown as a refusal", { skip: !fs.existsSync(KB) && "kb is not built" }, async () => {
    const r = await ask(tmp(), KB, "ls");
    assert.equal(r["kind"], "failed");
    const error = r["error"] as { code: string; noStore?: boolean };
    assert.equal(error.code, "not_found");
    assert.equal(error.noStore, true);
});

test("something missing inside a store stays a plain refusal", { skip: !fs.existsSync(KB) && "kb is not built" }, async () => {
    const dir = tmp();
    await new Kb({ bin: KB, cwd: dir, env: TEST_ENV }).init();
    const r = await ask(dir, KB, "get", { id: "D-99" });
    assert.equal(r["kind"], "failed");
    const error = r["error"] as { code: string; noStore?: boolean };
    assert.equal(error.code, "not_found");
    assert.equal(error.noStore, undefined);
});

test("a kb that is not there is reported as one that cannot start, with the command it was run as", async () => {
    const r = await ask(tmp(), "/nonexistent/kb", "ls");
    assert.equal(r["kind"], "crash");
    assert.equal(r["cannotStart"], "binary");
    assert.equal(r["command"], "/nonexistent/kb");
});
