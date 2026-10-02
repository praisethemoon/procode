/* The pending-edits badge: the count read from `lap status --json`, no badge
 * for nothing, and recounts that settle and never pile up. */

import * as assert from "node:assert/strict";
import { test } from "node:test";

import { Badge, PendingCount, badgeFor, pendingCount } from "../pending";

test("the count is the files lap status lists", () => {
    assert.equal(pendingCount('{"ok":true,"session":null,"files":[{"path":"a"},{"path":"b"}]}'), 2);
    assert.equal(pendingCount('{"ok":true,"session":"S1","files":[]}'), 0);
    assert.equal(pendingCount('{"ok":false,"error":"no_repo"}'), null);
    assert.equal(pendingCount("lap: unknown flag --json"), null);
    assert.equal(pendingCount(""), null);
});

test("no badge for nothing pending or no answer; one with a tooltip otherwise", () => {
    assert.equal(badgeFor(0), undefined);
    assert.equal(badgeFor(null), undefined);
    assert.deepEqual(badgeFor(1), { value: 1, tooltip: "1 file has edits lap has not recorded yet" });
    assert.deepEqual(badgeFor(1500), { value: 1500, tooltip: "1500 files have edits lap has not recorded yet" }, "VS Code shortens it, not us");
});

test("changes settle into one count, and a count is never asked twice at once", async () => {
    const shown: (Badge | undefined)[] = [];
    let asks = 0;
    let finish: ((s: string | null) => void) | null = null;
    const pc = new PendingCount(
        (done) => {
            asks++;
            finish = done;
        },
        (b) => shown.push(b),
        10,
    );
    pc.schedule();
    pc.schedule();
    pc.schedule();
    await new Promise((r) => setTimeout(r, 30));
    assert.equal(asks, 1, "three changes, one count");

    // Changes while lap is still counting ask for one more count, not three.
    pc.run();
    pc.run();
    pc.run();
    assert.equal(asks, 1);
    finish!('{"ok":true,"files":[{"path":"a"}]}');
    assert.equal(asks, 2, "one more count after the first");
    finish!('{"ok":true,"files":[]}');
    assert.equal(asks, 2);
    assert.deepEqual(shown, [{ value: 1, tooltip: "1 file has edits lap has not recorded yet" }, undefined]);

    // A failing lap clears the badge rather than leaving a stale number.
    pc.run();
    finish!(null);
    assert.equal(shown.at(-1), undefined);

    pc.dispose();
    pc.schedule();
    pc.run();
    assert.equal(asks, 3, "nothing after dispose");
});
