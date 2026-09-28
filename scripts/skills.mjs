/* The Agent Skills spec's rules (https://agentskills.io/specification),
 * checked for one skill folder. Pure but for reading the folder: it returns
 * the problems found, and the test decides what they mean.
 *
 * The frontmatter is read as the flat YAML the spec's examples use: one
 * `key: value` a line (a value may be quoted), and `metadata:` followed by
 * indented `key: value` lines. Anything else there is reported, not guessed
 * at. */

import * as fs from "node:fs";
import * as path from "node:path";

export const LIMITS = { name: 64, description: 1024, compatibility: 500, bodyLines: 500 };

/* `---` … `---` at the top of text: { fields, metadata, body, problems }. */
export function frontmatter(text) {
    const problems = [];
    const lines = text.split(/\r?\n/);
    if (lines[0] !== "---") return { fields: {}, metadata: null, body: text, problems: ["SKILL.md does not start with --- frontmatter"] };
    const end = lines.indexOf("---", 1);
    if (end < 0) return { fields: {}, metadata: null, body: text, problems: ["the frontmatter is not closed with ---"] };
    const fields = {};
    let metadata = null;
    for (const line of lines.slice(1, end)) {
        if (line.trim() === "") continue;
        const nested = /^\s+([^:\s][^:]*):\s*(.*)$/.exec(line);
        if (nested && metadata) {
            metadata[nested[1].trim()] = unquote(nested[2]);
            continue;
        }
        const m = /^([A-Za-z_][\w-]*):\s*(.*)$/.exec(line);
        if (!m) {
            problems.push(`frontmatter line not understood: ${line}`);
            continue;
        }
        if (m[1] === "metadata" && m[2] === "") {
            metadata = {};
            continue;
        }
        fields[m[1]] = unquote(m[2]);
    }
    return { fields, metadata, body: lines.slice(end + 1).join("\n"), problems };
}

function unquote(v) {
    const t = v.trim();
    return /^(["']).*\1$/.test(t) ? t.slice(1, -1) : t;
}

const NAME = /^[a-z0-9]+(-[a-z0-9]+)*$/;

/* The relative links in markdown outside fenced code: [text](target). */
export function links(body) {
    const out = [];
    let fenced = false;
    for (const line of body.split("\n")) {
        if (/^\s*```/.test(line)) fenced = !fenced;
        if (fenced) continue;
        for (const m of line.matchAll(/\]\(([^)\s]+)\)/g)) {
            const target = m[1].split("#")[0];
            if (target === "" || /^[a-z][a-z0-9+.-]*:/i.test(target)) continue; /* an anchor, a URL */
            out.push(target);
        }
    }
    return out;
}

/* Every problem with the skill in dir, as sentences; [] when it keeps the
 * spec's rules. */
export function checkSkill(dir) {
    const folder = path.basename(dir);
    const file = path.join(dir, "SKILL.md");
    if (!fs.existsSync(file)) return [`${folder}: no SKILL.md`];
    const fm = frontmatter(fs.readFileSync(file, "utf8"));
    const problems = fm.problems.map((p) => `${folder}: ${p}`);
    const say = (p) => problems.push(`${folder}: ${p}`);
    const { name, description, compatibility } = fm.fields;
    if (!name) say("no name");
    else {
        if (name.length > LIMITS.name) say(`name is ${name.length} characters, over ${LIMITS.name}`);
        if (!NAME.test(name)) say(`name "${name}" is not lowercase letters, digits and single hyphens`);
        if (name !== folder) say(`name "${name}" is not its folder's name`);
    }
    if (!description) say("no description");
    else if (description.length > LIMITS.description)
        say(`description is ${description.length} characters, over ${LIMITS.description}`);
    if (compatibility !== undefined && (compatibility.length < 1 || compatibility.length > LIMITS.compatibility))
        say(`compatibility is ${compatibility.length} characters, not 1-${LIMITS.compatibility}`);
    for (const [k, v] of Object.entries(fm.metadata ?? {}))
        if (typeof v !== "string" || v === "") say(`metadata ${k} has no string value`);
    const bodyLines = fm.body.split("\n").length;
    if (bodyLines >= LIMITS.bodyLines) say(`the body is ${bodyLines} lines, not under ${LIMITS.bodyLines}`);
    for (const target of links(fm.body)) {
        const norm = path.posix.normalize(target);
        if (path.isAbsolute(target) || norm.startsWith("..")) say(`links outside its folder: ${target}`);
        else if (norm.split("/").length > 2) say(`links deeper than one folder: ${target}`);
        else if (!fs.existsSync(path.join(dir, norm))) say(`links to a missing file: ${target}`);
    }
    return problems;
}
