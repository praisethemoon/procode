/* The skills under .claude/skills keep the Agent Skills spec's rules
 * (scripts/skills.mjs), and the checker catches a skill that breaks them. */

import * as assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";

import { checkSkill, frontmatter, links } from "./skills.mjs";

const SKILLS = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", ".claude", "skills");
const skills = fs.readdirSync(SKILLS).filter((d) => fs.statSync(path.join(SKILLS, d)).isDirectory());

test("every skill keeps the spec's rules and says what it needs to run", () => {
    assert.ok(skills.length >= 3);
    for (const s of skills) {
        assert.deepEqual(checkSkill(path.join(SKILLS, s)), [], s);
        const fm = frontmatter(fs.readFileSync(path.join(SKILLS, s, "SKILL.md"), "utf8"));
        assert.ok(fm.fields.compatibility, `${s} has no compatibility line`);
    }
});

test("skills-ref, the spec's own validator, agrees when it is installed", (t) => {
    let found = true;
    try {
        execFileSync("skills-ref", ["--help"], { stdio: "ignore" });
    } catch {
        found = false;
    }
    if (!found) {
        t.skip("skills-ref is not on PATH: only this repository's checks ran");
        return;
    }
    for (const s of skills) execFileSync("skills-ref", ["validate", path.join(SKILLS, s)], { stdio: "pipe" });
});

/* A skill folder in a throwaway directory: its SKILL.md, and other files. */
function fixture(folder, skillMd, files = {}) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "skills-"));
    const dir = path.join(root, folder);
    fs.mkdirSync(dir);
    fs.writeFileSync(path.join(dir, "SKILL.md"), skillMd);
    for (const [f, text] of Object.entries(files)) {
        fs.mkdirSync(path.dirname(path.join(dir, f)), { recursive: true });
        fs.writeFileSync(path.join(dir, f), text);
    }
    return { dir, done: () => fs.rmSync(root, { recursive: true, force: true }) };
}
const md = (name, description, body = "# body\n", extra = "") =>
    `---\nname: ${name}\ndescription: ${description}\n${extra}---\n${body}`;
const problems = (folder, text, files) => {
    const f = fixture(folder, text, files);
    try {
        return checkSkill(f.dir).join("\n");
    } finally {
        f.done();
    }
};

test("the checker catches a bad name, a long description, and links that break", () => {
    assert.equal(problems("good", md("good", "Does a thing.", "See [the notes](references/notes.md).\n"), { "references/notes.md": "n" }), "");
    assert.match(problems("Bad--Name", md("Bad--Name", "x")), /not lowercase letters, digits and single hyphens/);
    assert.match(problems("one", md("two", "x")), /is not its folder's name/);
    assert.match(problems("long", md("long", "d".repeat(1025))), /description is 1025 characters, over 1024/);
    assert.match(problems("miss", md("miss", "x", "[gone](references/gone.md)\n")), /links to a missing file: references\/gone.md/);
    assert.match(problems("out", md("out", "x", "[up](../other/SKILL.md)\n")), /links outside its folder/);
    assert.match(problems("deep", md("deep", "x", "[far](a/b/c.md)\n"), { "a/b/c.md": "c" }), /deeper than one folder/);
    assert.match(problems("comp", md("comp", "x", "# b\n", `compatibility: ${"c".repeat(501)}\n`)), /compatibility is 501 characters/);
    assert.match(problems("nofm", "# no frontmatter\n"), /does not start with ---/);
    assert.match(problems("big", md("big", "x", "line\n".repeat(500))), /the body is \d+ lines, not under 500/);
});

test("links() skips URLs, anchors and fenced code", () => {
    assert.deepEqual(links("[a](https://x.org) [b](#top) [c](ref.md#s)\n```\n[d](nope.md)\n```\n"), ["ref.md"]);
});
