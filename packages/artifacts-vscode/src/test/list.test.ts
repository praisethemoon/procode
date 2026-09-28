import * as assert from "node:assert/strict";
import { test } from "node:test";

import { describeRow } from "../list";

const NOW = Date.parse("2026-09-28T12:00:00Z");

test("a row says when it changed, then its keywords when it has some", () => {
    assert.equal(describeRow({ updatedAt: "2026-09-28T11:55:00Z", keywords: [] }, NOW), "5 min ago");
    assert.equal(describeRow({ updatedAt: "2026-09-28T11:55:00Z", keywords: ["lap", "merge"] }, NOW), "5 min ago · lap, merge");
});
