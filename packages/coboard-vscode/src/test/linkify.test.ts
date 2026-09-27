import * as assert from "node:assert/strict";
import { test } from "node:test";

import { commitRefs, linkTarget, linkifyIds } from "../linkify";

test("bare ids become in-board links", () => {
    assert.equal(linkifyIds("blocked by T-4, see E-1."), "blocked by [T-4](#T-4), see [E-1](#E-1).");
    assert.equal(linkifyIds("M-12"), "[M-12](#M-12)");
});

test("code, existing links and look-alikes are left alone", () => {
    assert.equal(linkifyIds("`T-4` and ```\nT-5\n```"), "`T-4` and ```\nT-5\n```");
    assert.equal(linkifyIds("[the ticket](#T-4)"), "[the ticket](#T-4)");
    assert.equal(linkifyIds("ET-4 T-0 T-4x UTF-8 X-1"), "ET-4 T-0 T-4x UTF-8 X-1");
});

test("only in-board hrefs are targets", () => {
    assert.equal(linkTarget("#T-4"), "T-4");
    assert.equal(linkTarget("https://example.com/#T-4"), null);
    assert.equal(linkTarget(undefined), null);
});

test("commit text names commits by a hash of 7 to 64 hex digits after #, or by id", () => {
    assert.deepEqual(commitRefs("Undoes #1A2B3C4 (L1029), then L7."), [
        "Undoes ",
        { ref: "1a2b3c4", text: "#1A2B3C4" },
        " (",
        { ref: "L1029", text: "L1029" },
        "), then ",
        { ref: "L7", text: "L7" },
        ".",
    ]);
    const full = "a".repeat(64);
    assert.deepEqual(commitRefs(`#${full}`), [{ ref: full, text: `#${full}` }]);
    assert.deepEqual(commitRefs("L2–L5"), [{ ref: "L2", text: "L2" }, "–", { ref: "L5", text: "L5" }]);
});

test("look-alikes are left as text", () => {
    for (const t of ["#abcdef", `#${"a".repeat(65)}`, "#abcdefg1", "a#abcdef0", "L0", "L12a", "HTML5", "XL1", "T-L1", "L1-2", "#L1"]) {
        assert.deepEqual(commitRefs(t), [t], t);
    }
    assert.deepEqual(commitRefs(""), []);
});
