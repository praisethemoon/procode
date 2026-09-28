import * as assert from "node:assert/strict";
import { test } from "node:test";

import { BESIDE, diffColumn } from "../placement";

test("from a panel, the diff opens in the column right of it, whatever was remembered", () => {
    assert.equal(diffColumn(1, undefined, []), 2, "first click: a new group beside the review");
    assert.equal(diffColumn(1, 2, [1, 2]), 2, "next clicks: that same group");
    assert.equal(diffColumn(1, 2, [1]), 2, "the group was closed: beside the review again");
    assert.equal(diffColumn(2, 2, [1, 2]), 3, "the review moved into the diff group: beside it, never its own group");
    assert.equal(diffColumn(3, 2, [1, 2, 3]), 4, "the review moved right: beside it");
});

test("a panel whose column is unknown is treated as no panel", () => {
    assert.equal(diffColumn(undefined, undefined, [1]), BESIDE);
    assert.equal(diffColumn(0, 2, [1, 2]), 2);
});

test("from a link inside a diff, the last diff group while it is open, else beside", () => {
    assert.equal(diffColumn(undefined, 2, [1, 2]), 2);
    assert.equal(diffColumn(undefined, 2, [1]), BESIDE);
    assert.equal(diffColumn(undefined, undefined, [1, 2]), BESIDE);
});
