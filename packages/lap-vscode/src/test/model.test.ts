/* Unit tests for the pure data layer, run with `node --test` (no VS Code
 * host needed). The extension layer is thin glue over this module.
 */
import * as assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { test } from "node:test";

import {
    consumableBytes,
    createReader,
    localTime,
    mdEscape,
    mdProse,
    parseLog,
    readerFeed,
    regionLabel,
    renderCommit,
    replaySeeded,
    shortHash,
    stateText,
    summaryLine,
} from "../model";

const sha = (line: string) => createHash("sha256").update(line, "utf8").digest("hex");

const INIT = `{"type":"init","version":1,"ts":"2026-09-20T10:00:00Z","prev":"0"}`;
const S1 = `{"type":"session_start","id":"S1","msg":"fix parser\\ndetails here","ts":"2026-09-20T10:00:01Z","prev":"a"}`;
const C1 = `{"type":"commit","id":"L1","session":"S1","file":"src/a.c","op":"create","old_start":1,"old_lines":0,"new_start":1,"new_lines":2,"eof_nl":true,"old_text":[],"new_text":["int x;","int y;"],"intent":"seed a.c","behavior":"Declares x and y","ts":"2026-09-20T10:00:02Z","prev":"b"}`;
const C2 = `{"type":"commit","id":"L2","session":"S1","file":"src/a.c","op":"edit","old_start":2,"old_lines":1,"new_start":2,"new_lines":1,"eof_nl":true,"old_text":["int y;"],"new_text":["long y;"],"intent":"widen y: overflow seen in prod","behavior":"Makes y a long","ts":"2026-09-20T10:00:03Z","prev":"c"}`;
const E1 = `{"type":"session_end","id":"S1","ts":"2026-09-20T10:00:04Z","prev":"d"}`;
const C3 = `{"type":"commit","id":"L3","session":null,"file":"notes.md","op":"delete","old_start":1,"old_lines":3,"new_start":1,"new_lines":0,"eof_nl":true,"old_text":["a","b","c"],"new_text":[],"intent":"notes retired","behavior":"Deletes the notes","ts":"2026-09-20T10:00:05Z","prev":"e"}`;
const S2 = `{"type":"session_start","id":"S2","msg":"second task","ts":"2026-09-20T10:00:06Z","prev":"f"}`;

function joined(...lines: string[]): string {
    return lines.join("\n") + "\n";
}

test("parses commits, sessions and their relationships", () => {
    const log = parseLog(joined(INIT, S1, C1, C2, E1, C3, S2), sha);
    assert.equal(log.commits.length, 3);
    assert.equal(log.sessions.length, 2);
    assert.equal(log.sessions[0].id, "S1");
    assert.equal(log.sessions[0].commits.length, 2);
    assert.equal(log.sessions[0].endTs, "2026-09-20T10:00:04Z");
    assert.equal(log.sessions[1].commits.length, 0);
    assert.equal(log.noSession.length, 1);
    assert.equal(log.noSession[0].id, "L3");
    assert.equal(log.activeSessionId, "S2");
    assert.equal(log.parseErrors, 0);
});

test("a commit carries its intent, its behavior, whether it was forced, and its line's hash", () => {
    const forced = C2.replace(`"behavior":"Makes y a long",`, `"behavior":"Makes y a long","forced":true,`);
    const log = parseLog(joined(INIT, S1, C1, forced), sha);
    const [c1, c2] = log.commits;
    assert.equal(c1.intent, "seed a.c");
    assert.equal(c1.behavior, "Declares x and y");
    assert.equal(c1.forced, false);
    assert.equal(c2.forced, true);
    assert.equal(c1.hash, sha(C1), "the SHA-256 of the record line, without its newline");
    assert.equal(c2.hash, sha(forced));
    assert.match(c1.hash, /^[0-9a-f]{64}$/);
    assert.equal(shortHash(c1.hash), c1.hash.slice(0, 7));
    assert.equal("msg" in c1, false, "a commit has no message field");
});

test("a commit's msg is not read", () => {
    const old = C1.replace(`"intent":"seed a.c","behavior":"Declares x and y",`, `"msg":"seed a.c",`);
    const c = parseLog(joined(INIT, S1, old), sha).commits[0];
    assert.equal(c.intent, "");
    assert.equal(c.behavior, "");
});

test("session_end clears the active session", () => {
    const log = parseLog(joined(INIT, S1, C1, E1), sha);
    assert.equal(log.activeSessionId, null);
});

test("a torn final line is tolerated and counted", () => {
    const text = joined(INIT, S1, C1) + `{"type":"commit","id":"L9","ses`;
    const log = parseLog(text, sha);
    assert.equal(log.commits.length, 1);
    assert.equal(log.parseErrors, 1);
});

test("empty and blank input parse to an empty log", () => {
    assert.equal(parseLog("", sha).commits.length, 0);
    assert.equal(parseLog("\n\n", sha).commits.length, 0);
});

test("mdEscape keeps ordinary spaces (word-wrap depends on this)", () => {
    const out = mdEscape("a long line with *stars* and _underscores_");
    assert.ok(!out.includes("&nbsp;"), "spaces must stay breakable");
    assert.ok(out.includes(" long line "), "plain spaces preserved");
    assert.ok(out.includes("\\*stars\\*"));
    assert.ok(out.includes("\\_underscores\\_"));
});

test("mdProse escapes per line and joins with hard breaks", () => {
    const out = mdProse("first [line]\nsecond -line");
    assert.equal(out, "first \\[line\\]  \nsecond \\-line");
    assert.ok(!out.includes("&nbsp;"));
});

test("summaryLine takes the first line only", () => {
    assert.equal(summaryLine("one\ntwo"), "one");
    assert.equal(summaryLine("single"), "single");
});

test("regionLabel mirrors the CLI shapes", () => {
    const log = parseLog(joined(INIT, S1, C1, C2, E1, C3), sha);
    assert.equal(regionLabel(log.commits[0]), "lines 1-2 (insertion)");
    assert.equal(regionLabel(log.commits[1]), "line 2");
    assert.equal(regionLabel(log.commits[2]), "lines 1-3 (deleted)");
});

test("renderCommit contains metadata, intent, behavior and diff lines", () => {
    const log = parseLog(joined(INIT, S1, C2, E1), sha);
    const text = renderCommit(log.commits[0]);
    assert.ok(text.startsWith(`commit L2 ${sha(C2)}  (session S1)\n`));
    assert.match(text, /intent:\n {2}widen y: overflow seen in prod\nbehavior:\n {2}Makes y a long\ndiff:/);
    assert.doesNotMatch(text, /forced/);
    assert.match(text, /file: src\/a\.c {2}\(edit\)/);
    assert.match(text, /@@ -2,1 \+2,1 @@/);
    assert.match(text, /- int y;/);
    assert.match(text, /\+ long y;/);
    const forced = parseLog(joined(INIT, S1, C2.replace(`"behavior":"Makes y a long",`, `"behavior":"Makes y a long","forced":true,`)), sha);
    assert.match(renderCommit(forced.commits[0]), /\nforced: the message checks were skipped \(--force-message\)\nintent:/);
});

test("replay reconstructs states across create/edit/delete", () => {
    const log = parseLog(joined(INIT, S1, C1, C2, E1, C3), sha);
    /* record indices: INIT=0 S1=1 C1=2 C2=3 E1=4 C3=5 */
    assert.equal(replaySeeded(log, "src/a.c", 1, null).exists, false);
    const afterCreate = replaySeeded(log, "src/a.c", 2, null);
    assert.deepEqual(afterCreate.lines, ["int x;", "int y;"]);
    assert.equal(afterCreate.exists, true);
    const afterEdit = replaySeeded(log, "src/a.c", 3, null);
    assert.deepEqual(afterEdit.lines, ["int x;", "long y;"]);
    /* a commit for ANOTHER file must not disturb this one */
    assert.deepEqual(replaySeeded(log, "src/a.c", 5, null).lines, [
        "int x;",
        "long y;",
    ]);
    const gone = replaySeeded(log, "notes.md", 5, null);
    assert.equal(gone.exists, false);
    assert.deepEqual(gone.lines, []);
});

test("stateText round-trips eof-newline state", () => {
    assert.equal(
        stateText({ lines: ["a", "b"], eofNl: true, exists: true }),
        "a\nb\n",
    );
    assert.equal(
        stateText({ lines: ["a", "b"], eofNl: false, exists: true }),
        "a\nb",
    );
    assert.equal(stateText({ lines: [], eofNl: true, exists: false }), "");
});

test("incremental feeding in arbitrary chunks equals one-shot parsing", () => {
    const text = joined(INIT, S1, C1, C2, E1, C3, S2);
    const whole = parseLog(text, sha);
    const r = createReader(sha);
    /* feed only at newline boundaries, in uneven groups (the extension
     * guarantees newline-terminated chunks) */
    const lines = text.split("\n").filter((l) => l.length > 0);
    readerFeed(r, lines.slice(0, 2).join("\n") + "\n");
    readerFeed(r, lines.slice(2, 3).join("\n") + "\n");
    readerFeed(r, lines.slice(3).join("\n") + "\n");
    assert.deepEqual(r.log.commits, whole.commits);
    assert.equal(r.log.sessions.length, whole.sessions.length);
    assert.equal(r.log.sessions[0].commits.length, 2);
    assert.equal(r.log.activeSessionId, whole.activeSessionId);
    assert.equal(r.log.records, whole.records);
});

test("recIndex counts every record line, all kinds", () => {
    const log = parseLog(joined(INIT, S1, C1, C2, E1, C3), sha);
    /* INIT=0 S1=1 C1=2 C2=3 E1=4 C3=5 */
    assert.equal(log.commits[0].recIndex, 2);
    assert.equal(log.commits[1].recIndex, 3);
    assert.equal(log.commits[2].recIndex, 5);
    assert.equal(log.records, 6);
});

test("replaySeeded equals birth replay, with and without snapshots", () => {
    const log = parseLog(joined(INIT, S1, C1, C2, E1), sha);
    const birth = replaySeeded(log, "src/a.c", 3, null);
    const unseeded = replaySeeded(log, "src/a.c", 3, null);
    assert.deepEqual(unseeded, birth);
    /* a snapshot at C1's record index (2): only C2 replays on top */
    const sidecar =
        JSON.stringify({ at: 2, eof_nl: true, content: "int x;\nint y;\n" }) +
        "\n";
    const seeded = replaySeeded(log, "src/a.c", 3, sidecar);
    assert.deepEqual(seeded, birth);
    /* a snapshot AFTER the target must be ignored */
    const late =
        JSON.stringify({ at: 9, eof_nl: true, content: "wrong\n" }) + "\n";
    assert.deepEqual(replaySeeded(log, "src/a.c", 3, late), birth);
    /* damaged sidecar lines never break correctness */
    assert.deepEqual(
        replaySeeded(log, "src/a.c", 3, "not json\n" + sidecar),
        birth,
    );
});

test("unknown record types and non-string session ids are ignored safely", () => {
    const weird =
        `{"type":"future_thing","x":1}\n` +
        `{"type":"commit","id":"L1","session":123,"file":"f","op":"edit",` +
        `"old_start":1,"old_lines":0,"new_start":1,"new_lines":1,` +
        `"eof_nl":true,"old_text":[],"new_text":["z"],"intent":"i","behavior":"b",` +
        `"ts":"t","prev":"p"}\n`;
    const log = parseLog(weird, sha);
    assert.equal(log.commits.length, 1);
    /* numeric session id is treated as no-session, not a crash */
    assert.equal(log.commits[0].session, null);
    assert.equal(log.noSession.length, 1);
});

test("consumableBytes stops at the last record boundary", () => {
    const enc = (s: string) => Buffer.from(s, "utf8");
    assert.equal(consumableBytes(enc("a\nb\n"), 4), 4);
    assert.equal(consumableBytes(enc("a\nbc"), 4), 2); /* partial held back */
    assert.equal(consumableBytes(enc("abc"), 3), 0);
    assert.equal(consumableBytes(enc("a\nbc"), 2), 2); /* short read */
});

test("incremental reader survives a torn tail then its repair", () => {
    /* The exact CLI sequence: append a record, crash mid-append leaving a
     * partial line, then a writer truncates the partial away and appends a
     * real record. A reader that consumed the partial bytes would read the
     * replacement from mid-record forever. */
    const r = createReader(sha);
    let offset = 0;
    const tick = (file: string): void => {
        if (file.length < offset) {
            return; /* truncation below our boundary: caller reparses */
        }
        const buf = Buffer.from(file.slice(offset), "utf8");
        const take = consumableBytes(buf, buf.length);
        if (take > 0) {
            readerFeed(r, buf.subarray(0, take).toString("utf8"));
            offset += take;
        }
    };

    let file = joined(INIT, S1, C1);
    tick(file);
    assert.equal(r.log.commits.length, 1);

    const boundary = file.length;
    file += `{"type":"commit","id":"L9","fi`; /* torn: no newline */
    tick(file);
    assert.equal(offset, boundary, "must not consume a partial record");
    assert.equal(r.log.commits.length, 1);

    file = file.slice(0, boundary) + C2 + "\n"; /* repair + real append */
    tick(file);
    assert.equal(r.log.commits.length, 2);
    assert.equal(r.log.commits[1].id, "L2");
    assert.equal(r.log.parseErrors, 0);
});

test("localTime shows the date, a space, and the time to the second", () => {
    const iso = "2026-09-27T09:36:48Z";
    const d = new Date(iso);
    const date = d.toLocaleDateString(undefined, { year: "numeric", month: "2-digit", day: "2-digit" });
    const shown = localTime(iso);
    assert.ok(shown.startsWith(`${date} `), shown);
    assert.match(shown.slice(date.length + 1), /^\d{1,2}:\d{2}:48\b/);
    assert.equal(localTime("not a time"), "not a time");
});
