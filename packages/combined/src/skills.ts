/* The skills procode ships (dist/skills/<name>/, copied from the repository's
 * .claude/skills at build time) and a project's copies of them in
 * .claude/skills/<name>/. No vscode here: the command and the notice in
 * extension.ts ask the person; this reads, compares and copies.
 *
 * A PROJECT'S OTHER SKILLS AND FILES ARE NEVER TOUCHED. Only a folder named
 * like one of ours is written, and only as a whole: replaced by the shipped
 * one, when the person said so. */

import * as crypto from "node:crypto";
import * as fs from "node:fs";
import * as path from "node:path";

export interface ShippedSkill {
    readonly name: string;
    /* metadata.version in its SKILL.md; "0" when it has none */
    readonly version: string;
    readonly dir: string;
}

const NAME = /^[a-z0-9]+(-[a-z0-9]+)*$/;

/* metadata.version from a SKILL.md's frontmatter. */
export function skillVersion(text: string): string {
    const fm = /^---\r?\n([\s\S]*?)\r?\n---/.exec(text)?.[1] ?? "";
    const meta = /^metadata:\s*\r?\n((?:[ \t]+.*\r?\n?)*)/m.exec(fm)?.[1] ?? "";
    return /^\s+version:\s*["']?([^"'\r\n]+?)["']?\s*$/m.exec(meta)?.[1] ?? "0";
}

/* The skills in dir (the package's skills/), by name. */
export function shippedSkills(dir: string): ShippedSkill[] {
    let names: string[];
    try {
        names = fs.readdirSync(dir);
    } catch {
        return [];
    }
    return names
        .filter((n) => NAME.test(n) && fs.existsSync(path.join(dir, n, "SKILL.md")))
        .sort()
        .map((name) => ({
            name,
            version: skillVersion(fs.readFileSync(path.join(dir, name, "SKILL.md"), "utf8")),
            dir: path.join(dir, name),
        }));
}

/* A folder's content as one hash: every file's relative path and bytes. */
export function folderHash(dir: string): string {
    const h = crypto.createHash("sha256");
    const walk = (rel: string): void => {
        for (const e of fs.readdirSync(path.join(dir, rel), { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
            const r = rel ? `${rel}/${e.name}` : e.name;
            if (e.isDirectory()) walk(r);
            else if (e.isFile()) h.update(`${r}\0`).update(fs.readFileSync(path.join(dir, r))).update("\0");
        }
    };
    walk("");
    return h.digest("hex");
}

export type SkillState = "absent" | "identical" | "different";

/* The project's copy of a shipped skill, next to the shipped one. */
export function stateOf(skill: ShippedSkill, projectSkills: string): SkillState {
    const mine = path.join(projectSkills, skill.name);
    if (!fs.existsSync(path.join(mine, "SKILL.md"))) return "absent";
    return folderHash(mine) === folderHash(skill.dir) ? "identical" : "different";
}

/* The project's folder for the skill, made the shipped one: anything that was
 * in that folder goes, nothing beside it is touched. Returns what it wrote,
 * for the record of what procode installed. */
export function install(skill: ShippedSkill, projectSkills: string): string {
    if (!NAME.test(skill.name)) throw new Error(`not a skill name: ${skill.name}`);
    const to = path.join(projectSkills, skill.name);
    fs.mkdirSync(projectSkills, { recursive: true });
    fs.rmSync(to, { recursive: true, force: true });
    fs.cpSync(skill.dir, to, { recursive: true });
    return folderHash(to);
}

/* On opening a project: which of our skills there are behind the shipped
 * ones. `installed` holds the hash of each folder as procode last wrote it.
 * A copy still exactly as written, of a lower version, can be updated; a
 * copy changed since (or never written by procode) is the person's, and is
 * only mentioned. */
export function behind(
    shipped: readonly ShippedSkill[],
    projectSkills: string,
    installed: Readonly<Record<string, string>>,
): { updatable: ShippedSkill[]; edited: ShippedSkill[] } {
    const updatable: ShippedSkill[] = [];
    const edited: ShippedSkill[] = [];
    for (const s of shipped) {
        const mine = path.join(projectSkills, s.name, "SKILL.md");
        if (!fs.existsSync(mine)) continue;
        const theirs = Number(skillVersion(fs.readFileSync(mine, "utf8")));
        if (!(theirs < Number(s.version))) continue;
        if (installed[s.name] === folderHash(path.join(projectSkills, s.name))) updatable.push(s);
        else edited.push(s);
    }
    return { updatable, edited };
}
