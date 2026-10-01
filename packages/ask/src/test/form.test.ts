import * as assert from "node:assert/strict";
import { test } from "node:test";

import { AskError, parseAnswer, parseSteps, results } from "../form";

function refused(f: () => unknown, re: RegExp): void {
    assert.throws(f, (e: unknown) => e instanceof AskError && e.code === "invalid" && re.test(e.message));
}

test("steps are normalised: ids from position, kinds from options, bare labels", () => {
    const steps = parseSteps([
        { title: "Which layout?", options: ["Sidebar", { label: "Tab", description: "more room", preview: "<b>x</b>" }] },
        { title: "Anything else?" },
        { id: "langs", title: "Languages", kind: "multi", options: ["C", "TS"], other: false },
    ]);
    assert.deepEqual(steps, [
        {
            id: "1",
            title: "Which layout?",
            kind: "single",
            options: [{ label: "Sidebar" }, { label: "Tab", description: "more room", preview: "<b>x</b>" }],
        },
        { id: "2", title: "Anything else?", kind: "text" },
        { id: "langs", title: "Languages", kind: "multi", options: [{ label: "C" }, { label: "TS" }], other: false },
    ]);
});

test("bad steps are refused with where they went wrong", () => {
    refused(() => parseSteps([]), /at least one question/);
    refused(() => parseSteps([{ title: "" }]), /steps\[0\]\.title is empty/);
    refused(() => parseSteps([{ title: "a", kind: "pick" }]), /kind must be one of single, multi, text/);
    refused(() => parseSteps([{ title: "a", kind: "single" }]), /needs options/);
    refused(() => parseSteps([{ title: "a", kind: "text", options: ["x"] }]), /takes no options/);
    refused(() => parseSteps([{ title: "a", options: ["x"], other: false }]), /at least two options/);
    refused(() => parseSteps([{ title: "a", options: ["x", "x"] }]), /two options labelled "x"/);
    refused(() => parseSteps([{ id: "q", title: "a" }, { id: "q", title: "b" }]), /two steps have the id "q"/);
    refused(() => parseSteps([{ title: "a", options: [3] }]), /options\[0\] must be a label/);
});

test("one option is enough while Other is on", () => {
    assert.equal(parseSteps([{ title: "a", options: ["Yes"] }])[0].options!.length, 1);
});

test("answers are checked, and a step never touched reads as skipped", () => {
    const req = { id: "F-1", title: "t", createdAt: "", steps: parseSteps([{ title: "a", options: ["x", "y"] }, { title: "b" }, { title: "c" }]) };
    const ans = parseAnswer({
        status: "submitted",
        answeredAt: "now",
        steps: { "1": { state: "answered", choices: ["y"], note: "  " }, "2": { state: "needs_more", question: "which file?" } },
    });
    assert.deepEqual(results(req, ans), [
        { id: "1", title: "a", state: "answered", choices: ["y"] },
        { id: "2", title: "b", state: "needs_more", question: "which file?" },
        { id: "3", title: "c", state: "skipped" },
    ]);
    refused(() => parseAnswer({ status: "done" }), /submitted or cancelled/);
    refused(() => parseAnswer({ status: "submitted", steps: { "1": { state: "maybe" } } }), /state must be one of/);
    refused(() => parseAnswer({ status: "submitted", steps: { "1": { state: "answered", choices: "x" } } }), /list of labels/);
});
