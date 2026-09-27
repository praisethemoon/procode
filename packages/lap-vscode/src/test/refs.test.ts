/* References in a commit's text: what is one, how it becomes a link, and
 * how it resolves against the log. */

import * as assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";

import { parseLog } from "../model";
import { mdLinked, parseRefs, resolveRef } from "../refs";

const sha = (line: string) => createHash("sha256").update(line, "utf8").digest("hex");
const refs = (text: string) => parseRefs(text).filter((p) => p.ref !== undefined).map((p) => p.ref);

test("a # and 7 to 64 hex digits, or a commit id, is a reference", () => {
    assert.deepEqual(refs("needs #fa9cebd, fixes #FA9CEBD01 and reverts L1029."), ["#fa9cebd", "#FA9CEBD01", "L1029"]);
    assert.deepEqual(refs("#" + "a".repeat(64)), ["#" + "a".repeat(64)]);
    assert.deepEqual(refs("(L7) [#0123456]"), ["L7", "#0123456"]);
});

test("a token that runs into other characters is not a reference", () => {
    assert.deepEqual(refs("#fa9ceb"), [], "6 digits");
    assert.deepEqual(refs("#" + "a".repeat(65)), [], "65 digits");
    assert.deepEqual(refs("#fa9cebdz a#fa9cebd ##fa9cebd #ga9cebd"), []);
    assert.deepEqual(refs("HTML5 L12x xL12 l12 S3 T-69 L-1"), [], "ids stand alone, and only commits' ids");
});

test("the parts put back together are the text", () => {
    for (const t of ["", "no refs at all", "L1", "see #abcdef0 then L2 and L3", "#abcdef0L1"]) {
        assert.equal(parseRefs(t).map((p) => p.text).join(""), t);
    }
    assert.deepEqual(parseRefs("see L2 then #abcdef0"), [
        { text: "see " },
        { text: "L2", ref: "L2" },
        { text: " then " },
        { text: "#abcdef0", ref: "#abcdef0" },
    ]);
    assert.deepEqual(parseRefs(""), []);
});

test("mdLinked escapes prose and links each reference", () => {
    const out = mdLinked("fixes *L2* (see #abcdef0)\nsecond_line", (r) => `command:x?${encodeURIComponent(r)}`);
    assert.equal(out, "fixes \\*[L2](command:x?L2)\\* \\(see [\\#abcdef0](command:x?%23abcdef0)\\)  \nsecond\\_line");
});

const C = (id: string, n: number) =>
    JSON.stringify({ type: "commit", id, session: null, file: "a", op: "edit", old_start: 1, old_lines: 1, new_start: 1, new_lines: 1,
        eof_nl: true, old_text: ["a"], new_text: [String(n)], intent: "i", behavior: "b", ts: "t" });

test("a reference resolves as lap show does: an id, or a unique hash prefix of 7 or more", () => {
    const lines = Array.from({ length: 40 }, (_, i) => C(`L${i + 1}`, i));
    const log = parseLog(lines.join("\n") + "\n", sha);
    const l5 = log.commits[4];
    assert.deepEqual(resolveRef(log, "L5"), { ok: true, id: "L5" });
    assert.deepEqual(resolveRef(log, "l5"), { ok: true, id: "L5" });
    assert.deepEqual(resolveRef(log, "#" + l5.hash.slice(0, 7)), { ok: true, id: "L5" });
    assert.deepEqual(resolveRef(log, l5.hash.toUpperCase()), { ok: true, id: "L5" });
    assert.deepEqual(resolveRef(log, "L41"), { ok: false, error: "unknown_ref", matches: [] });
    assert.deepEqual(resolveRef(log, "#" + l5.hash.slice(0, 6)), { ok: false, error: "unknown_ref", matches: [] });
    const miss = l5.hash.slice(0, 6) + (l5.hash[6] === "0" ? "1" : "0");
    assert.equal(resolveRef(log, miss).ok, false);

    /* Two commits whose hashes share their first 7 digits. */
    const twins = { ...log, commits: log.commits.slice(0, 2).map((c) => ({ ...c, hash: "abcdef0" + c.hash.slice(7) })) };
    assert.deepEqual(resolveRef(twins, "#abcdef0"), { ok: false, error: "ambiguous_ref", matches: ["L1", "L2"] });
    assert.deepEqual(resolveRef(twins, twins.commits[1].hash.slice(0, 12)), { ok: true, id: "L2" });
});
