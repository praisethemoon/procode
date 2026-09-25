import * as assert from "node:assert/strict";
import { test } from "node:test";

import { linkTarget, linkifyIds } from "../linkify";

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
