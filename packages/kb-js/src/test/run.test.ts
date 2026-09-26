/* The process boundary: what crosses it, and what each exit code becomes.
 *
 * A REAL CHILD PROCESS, NOT A MOCKED `spawn`. The failures this file is about
 * only exist at the boundary — an argument that arrives as two arguments, a
 * document that never reaches stdin, an exit code read as the wrong kind of
 * failure — and a mock would answer "fine" to every one of them.
 */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { KbCrash, KbError } from "../errors";
import { run } from "../run";
import { FakeKb, ok, refusal } from "./fake";

async function withFake<T>(
    answers: Parameters<typeof FakeKb.create>[0],
    fn: (fake: FakeKb) => Promise<T>,
): Promise<T> {
    const fake = FakeKb.create(answers);
    try {
        return await fn(fake);
    } finally {
        fake.dispose();
    }
}

test("a success is the payload, envelope and all", async () => {
    await withFake([{ stdout: ok({ documents: [], count: 0 }) }], async (fake) => {
        const payload = await run(["ls"], { bin: fake.bin, env: fake.env() });
        assert.deepEqual(payload, { ok: true, documents: [], count: 0 });
    });
});

test("--json is appended exactly once, whatever the command", async () => {
    /* No builder writes it, this writes it, and a second copy would be a flag
     * the CLI's parser has to make a decision about. */
    await withFake([{ stdout: ok({}) }, { stdout: ok({}) }], async (fake) => {
        await run(["ls", "--collection", "x"], { bin: fake.bin, env: fake.env() });
        await run(["status"], { bin: fake.bin, env: fake.env() });
        for (const call of fake.calls()) {
            assert.equal(
                call.argv.filter((a) => a === "--json").length,
                1,
                `--json appeared ${call.argv.filter((a) => a === "--json").length} times in ${JSON.stringify(call.argv)}`,
            );
            assert.equal(call.argv[call.argv.length - 1], "--json");
        }
    });
});

test("--json goes in front of a -- separator, where kb still reads it as a flag", async () => {
    /* Past `--` every argument is a positional, so a trailing --json would be
     * read as a name and the answer would come back as text: renaming or
     * deleting a collection, and searching for a query that starts with a
     * dash, all put `--` in front of their subject. */
    await withFake([{ stdout: ok({}) }, { stdout: ok({ hits: [] }) }], async (fake) => {
        await run(["collections", "delete", "--", "io-uring"], { bin: fake.bin, env: fake.env() });
        await run(["search", "--", "-O2"], { bin: fake.bin, env: fake.env() });
        const [del, search] = fake.calls();
        assert.deepEqual(del.argv, ["collections", "delete", "--json", "--", "io-uring"]);
        assert.deepEqual(search.argv, ["search", "--json", "--", "-O2"]);
    });
});

test("a hostile value arrives as one argument, with its metacharacters in it", async () => {
    /* THE CLAIM THE WHOLE PACKAGE MAKES. There is no command string anywhere
     * in it, so a query a reader typed cannot become syntax. Stated against a
     * real `process.argv` rather than against the array this package built,
     * because the thing that would break it is a shell between the two. */
    const hostile = 'io_uring; rm -rf ~/dummy && echo "$(whoami)" > /tmp/pwned';
    await withFake([{ stdout: ok({ hits: [] }) }], async (fake) => {
        await run(["search", hostile], { bin: fake.bin, env: fake.env() });
        const [call] = fake.calls();
        assert.deepEqual(call.argv, ["search", hostile, "--json"]);
        assert.equal(
            call.argv.filter((a) => a === hostile).length,
            1,
            "the query did not arrive whole",
        );
    });
});

test("stdin carries the document, and the argv does not", async () => {
    const content = "# IOCP\n\nCreateIoCompletionPort associates a handle with a port.\n";
    await withFake([{ stdout: ok({ document: "D-1" }) }], async (fake) => {
        await run(["add", "--title", "IOCP", "--file", "-"], { bin: fake.bin, env: fake.env() }, content);
        const [call] = fake.calls();
        assert.equal(call.stdin, content);
        assert.ok(
            !call.argv.some((a) => a.includes("CreateIoCompletionPort")),
            "the document's text was passed in the argument list",
        );
    });
});

test("a command with no stdin gets a closed one rather than an open pipe", async () => {
    /* A child that reads to end-of-file on stdin — which `kb add --file -`
     * does — would wait for ever if this left the pipe open, and the caller
     * would see a timeout with no reason in it. */
    await withFake([{ stdout: ok({}) }], async (fake) => {
        await run(["status"], { bin: fake.bin, env: fake.env() });
        assert.equal(fake.calls()[0].stdin, "");
    });
});

test("the working directory is the child's, because it decides which store is found", async () => {
    /* §1.4 walks up from the working directory to find the project store, so
     * a binding that left `cwd` to chance would read a different store
     * depending on how the editor was launched. */
    await withFake([{ stdout: ok({}) }], async (fake) => {
        await run(["status"], { bin: fake.bin, cwd: fake.dir, env: fake.env() });
        const { realpathSync } = await import("node:fs");
        assert.equal(realpathSync(fake.calls()[0].cwd), realpathSync(fake.dir));
    });
});

test("the environment crosses, which is how HOME reaches the model lookup", async () => {
    /* The CLI reads one variable, HOME, to find ~/.kb/models (§8), and a
     * binding that built a fresh environment would silently drop it — the
     * store would then answer as if no model were installed. */
    await withFake([{ stdout: ok({}) }], async (fake) => {
        await run(["status"], { bin: fake.bin, env: fake.env({ HOME: "/elsewhere/home" }) });
        assert.equal(fake.calls()[0].env.HOME, "/elsewhere/home");
    });
});

/* ------------------------------------------------------- the three codes */

test("exit 1 is a refusal, carrying the store's own code", async () => {
    await withFake([{ stdout: refusal("not_found", "no document D-9999"), exit: 1 }], async (fake) => {
        const e = await run(["get", "D-9999"], { bin: fake.bin, env: fake.env() }).then(
            () => null,
            (x: unknown) => x,
        );
        assert.ok(e instanceof KbError, `expected a KbError, got ${String(e)}`);
        assert.equal(e.code, "not_found");
        assert.equal(e.spec, "not_found");
        assert.equal(e.unrecognised, false);
        assert.equal(e.message, "no document D-9999");
        assert.deepEqual([...e.argv], ["get", "D-9999", "--json"]);
    });
});

test("a code §11 does not have is kept as the store's word and flagged", async () => {
    /* `usage` and `corrupt_log` are real exit-1 refusals the CLI makes and are
     * in neither §11's table nor anybody's guess at it. Translating one onto
     * `not_found` would answer a question nobody asked. */
    await withFake([{ stdout: refusal("usage", 'unknown option "--nope"'), exit: 1 }], async (fake) => {
        const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbError;
        assert.ok(e instanceof KbError);
        assert.equal(e.code, "usage");
        assert.equal(e.spec, null, "a CLI code was reported as one of §11's");
        assert.equal(e.unrecognised, false, "usage is a code this binding knows about");
    });
    await withFake([{ stdout: refusal("wormhole", "something new"), exit: 1 }], async (fake) => {
        const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbError;
        assert.equal(e.code, "wormhole");
        assert.equal(e.spec, null);
        assert.equal(e.unrecognised, true, "a code from neither list was not flagged as unknown");
    });
});

test("an error's details are read typed for their own code, and only when they have its shape", async () => {
    const envelope = (details: unknown): string =>
        JSON.stringify({ ok: false, error: "collection_in_use", message: "still holds 3", details }) + "\n";
    await withFake([{ stdout: envelope({ collection: "win32", documents: 3 }), exit: 1 }], async (fake) => {
        const e = (await run(["collections", "delete", "win32"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbError;
        assert.deepEqual(e.details, { collection: "win32", documents: 3 });
        assert.equal(e.detailsOf("collection_in_use")?.documents, 3);
        assert.equal(e.detailsOf("store_locked"), null, "details were read under another code");
    });
    await withFake([{ stdout: envelope({ collection: "win32", documents: "three" }), exit: 1 }], async (fake) => {
        const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbError;
        assert.equal(e.detailsOf("collection_in_use"), null, "a mis-shaped details object was typed anyway");
    });
    await withFake([{ stdout: refusal("not_found", "no document D-9"), exit: 1 }], async (fake) => {
        const e = (await run(["get", "D-9"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbError;
        assert.equal(e.details, null);
    });
});

test("exit 2 is a fault and is never dressed as a refusal", async () => {
    /* §11's table is the vocabulary a caller can act on, and a bug in kb is
     * not in it. A UI that showed this as an error would tell the reader to
     * correct input that was never the problem. */
    await withFake(
        [{ stdout: '{"ok":false,"error":"internal","message":"cannot resolve path"}\n', exit: 2 }],
        async (fake) => {
            const e = (await run(["add"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbCrash;
            assert.ok(e instanceof KbCrash, `expected a KbCrash, got ${String(e)}`);
            assert.equal(e.exitCode, 2);
            assert.match(e.message, /cannot resolve path/);
        },
    );
});

test("a zero exit this reader cannot parse is a fault, not an empty answer", async () => {
    /* Answering `{}` would be a UI drawing an empty list over a binary that
     * said something else — the failure mode where nothing is wrong on screen
     * and nothing is right either. */
    for (const stdout of ["", "not json at all", "[1,2,3]", '{"ok":false}', '{"documents":[]}']) {
        await withFake([{ stdout }], async (fake) => {
            const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbCrash;
            assert.ok(e instanceof KbCrash, `${JSON.stringify(stdout)} did not become a crash`);
            assert.equal(e.exitCode, 0);
        });
    }
});

test("an exit 1 with no envelope is a fault, because there is no §11 code to invent", async () => {
    await withFake([{ stdout: "usage: kb <command>\n", stderr: "", exit: 1 }], async (fake) => {
        const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbCrash;
        assert.ok(e instanceof KbCrash);
        assert.equal(e.exitCode, 1);
    });
    /* And a refusal that names no code at all is the same thing: a payload
     * this binding cannot type. */
    await withFake([{ stdout: '{"ok":false,"message":"nope"}\n', exit: 1 }], async (fake) => {
        const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbCrash;
        assert.ok(e instanceof KbCrash);
    });
});

test("a binary that is not there says so, naming what it looked for, and that it never started", async () => {
    const e = (await run(["status"], { bin: "/nonexistent/kb-binary" }).catch((x: unknown) => x)) as KbCrash;
    assert.ok(e instanceof KbCrash);
    assert.equal(e.exitCode, null);
    assert.equal(e.cannotStart, "binary");
    assert.match(e.message, /not found/);
    assert.match(e.message, /kb-binary/);
    const bare = (await run(["status"], { bin: "kb-binary-that-is-nowhere" }).catch((x: unknown) => x)) as KbCrash;
    assert.equal(bare.cannotStart, "binary");
    assert.match(bare.message, /on the PATH/, "a bare name was looked for on the PATH, and says so");
});

test("a folder that is not there is not reported as a missing binary", async () => {
    /* Node answers ENOENT for both; a reader told to fix their PATH when the
     * folder was the problem is sent the wrong way. */
    const e = (await run(["status"], { bin: process.execPath, cwd: "/nonexistent/folder-for-kb" }).catch((x: unknown) => x)) as KbCrash;
    assert.ok(e instanceof KbCrash);
    assert.equal(e.cannotStart, "folder");
    assert.match(e.message, /folder-for-kb does not exist/);
});

test("a crash after kb started is not a failure to start", async () => {
    await withFake([{ stdout: "", stderr: "boom", exit: 2 }], async (fake) => {
        const e = (await run(["ls"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbCrash;
        assert.equal(e.cannotStart, null);
    });
});

test("a command that does not answer is killed and reported, not awaited for ever", async () => {
    /* §2 of the UI spec searches as the reader types. A search that never
     * comes back must not leave the box waiting on it. */
    await withFake([{ stdout: ok({}), delayMs: 5000 }], async (fake) => {
        const started = Date.now();
        const e = (await run(["search", "x"], {
            bin: fake.bin,
            env: fake.env(),
            timeoutMs: 150,
        }).catch((x: unknown) => x)) as KbCrash;
        assert.ok(e instanceof KbCrash, `expected a crash, got ${String(e)}`);
        assert.match(e.message, /did not answer within 150ms/);
        assert.ok(Date.now() - started < 4000, "the timeout did not fire");
    });
});

test("an answer larger than the reader will hold is refused rather than buffered", async () => {
    await withFake([{ stdout: "x".repeat(4096) }], async (fake) => {
        const e = (await run(["get", "D-1"], {
            bin: fake.bin,
            env: fake.env(),
            maxBytes: 512,
        }).catch((x: unknown) => x)) as KbCrash;
        assert.ok(e instanceof KbCrash);
        assert.match(e.message, /more than 512 bytes/);
    });
});

test("stderr travels with a fault, because it is the only place left to look", async () => {
    await withFake([{ stdout: "", stderr: "error: the store is on fire\n", exit: 2 }], async (fake) => {
        const e = (await run(["status"], { bin: fake.bin, env: fake.env() }).catch((x: unknown) => x)) as KbCrash;
        assert.ok(e instanceof KbCrash);
        assert.equal(e.stderr, "error: the store is on fire");
    });
});
