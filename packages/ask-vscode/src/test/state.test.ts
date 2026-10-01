import * as assert from "node:assert/strict";
import { test } from "node:test";

import { parseSteps, type Request } from "ask";

import {
    FormState,
    answeredCount,
    back,
    draftOf,
    go,
    initial,
    keyAction,
    next,
    pick,
    pickOther,
    setField,
    shownState,
    skip,
    toAnswer,
} from "../state";

const req: Request = {
    id: "F-1",
    title: "The ask tab",
    createdAt: "",
    steps: parseSteps([
        { id: "where", title: "Where?", options: ["Sidebar", "Tab"] },
        { id: "langs", title: "Languages", kind: "multi", options: ["C", "TS", "Go"] },
        { id: "why", title: "Why?" },
        { id: "fixed", title: "Pick one", options: ["A", "B"], other: false },
    ]),
};
const [where, langs, why] = req.steps;

const st = (s: FormState, i: number) => shownState(req.steps[i], draftOf(s, req.steps[i]));

test("every step starts open, and the form starts on the first", () => {
    const s = initial(req);
    assert.equal(s.place, 0);
    assert.deepEqual(req.steps.map((_, i) => st(s, i)), ["open", "open", "open", "open"]);
    assert.equal(answeredCount(req, s), 0);
});

test("a single step holds one choice, Other replacing it; a multi step toggles", () => {
    let s = pick(initial(req), where, "Sidebar");
    s = pick(s, where, "Tab");
    assert.deepEqual(draftOf(s, where).choices, ["Tab"]);
    s = pickOther(s, where);
    assert.deepEqual(draftOf(s, where).choices, []);
    assert.equal(st(s, 0), "open", "Other picked but empty is not an answer");
    s = setField(s, where, "other", "a panel");
    assert.equal(st(s, 0), "answered");
    s = pick(s, where, "Tab");
    assert.equal(draftOf(s, where).otherOn, false);

    s = pick(pick(pick(s, langs, "C"), langs, "Go"), langs, "C");
    assert.deepEqual(draftOf(s, langs).choices, ["Go"]);
    s = setField(s, langs, "other", "Zig");
    assert.deepEqual(toAnswer(req, s).steps["langs"], { state: "answered", choices: ["Go"], other: "Zig" });
});

test("asking for more wins over an answer, and both go back", () => {
    let s = setField(initial(req), why, "text", "speed");
    s = setField(s, why, "more", "  speed of what?  ");
    assert.equal(st(s, 2), "needs_more");
    assert.deepEqual(toAnswer(req, s).steps["why"], { state: "needs_more", text: "speed", question: "speed of what?" });
});

test("skipping sets an answer aside without erasing it; touching the step brings it back", () => {
    let s = pick(initial(req), where, "Tab");
    s = setField(s, where, "note", "for now");
    s = skip(s, where);
    assert.equal(st(s, 0), "skipped");
    assert.deepEqual(toAnswer(req, s).steps["where"], { state: "skipped", note: "for now" });
    s = setField(s, where, "note", "for now, really");
    assert.equal(st(s, 0), "answered");
    assert.deepEqual(draftOf(s, where).choices, ["Tab"]);
});

test("open steps are sent as skipped, with nothing else", () => {
    const s = setField(initial(req), why, "note", "a note alone is not an answer");
    const a = toAnswer(req, s, new Date("2026-10-02T00:00:00Z"));
    assert.equal(a.status, "submitted");
    assert.equal(a.answeredAt, "2026-10-02T00:00:00.000Z");
    assert.deepEqual(a.steps["why"], { state: "skipped" });
    assert.deepEqual(Object.keys(a.steps), ["where", "langs", "why", "fixed"]);
});

test("answers carried over from an earlier form fill the steps in", () => {
    const s = initial({
        ...req,
        previous: { where: { state: "answered", choices: ["Tab", "Gone"] }, why: { state: "answered", text: "x", note: "n" } },
    });
    assert.deepEqual(draftOf(s, where).choices, ["Tab"], "a label no longer offered is dropped");
    assert.equal(draftOf(s, why).text, "x");
    assert.equal(draftOf(s, why).note, "n");
    assert.equal(answeredCount(req, s), 2);
});

test("next and back walk the steps, then the review page", () => {
    let s = initial(req);
    for (let i = 0; i < 4; i++) s = next(req, s);
    assert.equal(s.place, "review");
    assert.equal(next(req, s).place, "review");
    assert.equal(back(req, s).place, 3);
    assert.equal(back(req, go(s, 0)).place, 0);
});

test("keys: digits pick, the one after the options is Other, Enter moves, typing is left alone", () => {
    const k = (key: string, more: Partial<{ shift: boolean; mod: boolean; typing: boolean }> = {}) => ({ key, shift: false, mod: false, typing: false, ...more });
    assert.deepEqual(keyAction(req, 0, k("2")), { do: "pick", label: "Tab" });
    assert.deepEqual(keyAction(req, 0, k("3")), { do: "other" });
    assert.equal(keyAction(req, 0, k("4")), null);
    assert.equal(keyAction(req, 3, k("3")), null, "no Other on a step that turned it off");
    assert.equal(keyAction(req, 2, k("1")), null, "a text step has no options");
    assert.deepEqual(keyAction(req, 0, k("Enter")), { do: "next" });
    assert.deepEqual(keyAction(req, 1, k("Enter", { shift: true })), { do: "back" });
    assert.equal(keyAction(req, 0, k("2", { typing: true })), null);
    assert.equal(keyAction(req, 0, k("Enter", { typing: true })), null);
    assert.deepEqual(keyAction(req, 0, k("Enter", { typing: true, mod: true })), { do: "next" });
    assert.deepEqual(keyAction(req, "review", k("Enter", { mod: true })), { do: "submit" });
    assert.equal(keyAction(req, "review", k("Enter")), null, "plain Enter never submits");
    assert.deepEqual(keyAction(req, 0, k("Escape", { typing: true })), { do: "blur" });
});
