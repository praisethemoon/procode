/* techdocs as MCP tools, over stdio: newline-delimited JSON-RPC 2.0
 * (specs/techdocs.md §4).
 *
 * Agents publish a page, list what is there and read one back. Deleting is
 * left to people, in the editor.
 *
 * The pages are the `.techdocs/` found by walking up from the directory the
 * server was started in (or a store from before the rename, moved on the
 * first publish); with none, the first publish creates one at the enclosing
 * git repository's root.
 */

import * as readline from "node:readline";

import { Techdocs, TechdocsError, MAX_DESCRIPTION, MAX_KEYWORD, MAX_KEYWORDS, MAX_TITLE, defaultRoot, findTechdocs, hasKeyword } from "./store";
import { TEMPLATES } from "./templates";

const VERSION = "0.2.0";

/* What an agent needs to write a page that looks right, said once, where the
 * agent reads it. The token list is the useful subset of baukasten's; the
 * spec names the rest. */
export const INSTRUCTIONS = `These are local pages shown in VS Code by procode's techdocs — not claude.ai artifacts, and not your own Artifact tool.

techdocs pages are finished pieces of work you hand to the person as a page — a report, a comparison, a design note, findings, a chart. Publish one with techdocs_publish when the result is worth reading as a document rather than as chat.

The page is HTML (a whole document or a fragment). It is shown inside VS Code in the person's theme, so style it ONLY with baukasten's CSS variables, never with fixed colours:
- colour: --bk-color-foreground, --bk-color-foreground-muted, --bk-color-background, --bk-color-background-secondary, --bk-color-background-elevated, --bk-color-border, --bk-color-divider, --bk-color-link, --bk-color-primary, --bk-color-primary-foreground, --bk-color-success, --bk-color-warning, --bk-color-danger, --bk-color-info, --bk-color-code-background, --bk-color-code-foreground
- space: --bk-spacing-1 (0.25rem) … --bk-spacing-24, --bk-gap-xs|sm|md|lg|xl
- type: --bk-font-family-sans, --bk-font-family-mono, --bk-font-size-xs|sm|md|base|lg|xl|2xl|3xl, --bk-font-weight-normal|medium|semibold|bold, --bk-line-height-tight|normal|relaxed
- shape: --bk-radius-sm|md|lg, --bk-border-width-1|2, --bk-shadow-sm|md
Plain elements (headings, paragraphs, lists, tables, code, pre, blockquote, details, links) are already styled, and so are components by class name, so a report needs no CSS at all:
eyebrow, lede · meta + chip · kpis + kpi (<b>number</b><span>what</span><small>why</small>; kpi.warn, kpi.danger) · callout ok|info|warn|danger (<strong>verdict</strong><p>reason</p>) · cols, panel · tag (ok|warn|danger) · tabs (buttons with aria-pressed) · td.id, td.num · toc · svg.chart with .bar (ok|warn|danger|muted), .grid, .node (accent), .edge (accent|dashed), .arrowhead, text.label.
For anything longer than a few paragraphs, start from techdocs_template {name: "report"}: it has every component in place and says what goes where.
The page has no network: inline everything (SVG, data: images, scripts). Scripts run sandboxed.
Give every page two to five keywords (short lowercase words or phrases: the topic, the component, the kind of page) so pages about one topic can be found together; techdocs_list filters by one.
Republish with the same id to revise a page; its createdAt is kept, and so are its keywords unless you pass new ones.`;

type Json = Record<string, unknown>;

interface Tool {
    readonly name: string;
    readonly description: string;
    readonly inputSchema: Json;
    readonly call: (args: Json, ctx: Ctx) => unknown;
}

export interface Ctx {
    readonly cwd: string;
}

function readStore(ctx: Ctx): Techdocs | null {
    const root = findTechdocs(ctx.cwd);
    return root ? new Techdocs(root) : null;
}

export const TOOLS: readonly Tool[] = [
    {
        name: "techdocs_publish",
        description:
            "Publish an HTML page the person reads in VS Code. Without id, creates the next A-<n>; with id, replaces that page and its metadata. Style with baukasten's --bk-* variables only (see the server instructions). Returns the artifact and the path of its page.",
        inputSchema: {
            type: "object",
            properties: {
                title: { type: "string", description: `One line, at most ${MAX_TITLE} characters.` },
                description: {
                    type: "string",
                    description: `What the page is and why it exists; shown under the title in the list. At most ${MAX_DESCRIPTION} characters.`,
                },
                html: { type: "string", description: "The page: a whole HTML document or a fragment." },
                keywords: {
                    type: "array",
                    items: { type: "string" },
                    description: `Two to five short words or phrases the page is about, e.g. ["lap", "merge", "design note"]; stored lowercase, at most ${MAX_KEYWORDS}, each at most ${MAX_KEYWORD} characters. With id: given, they replace the page's keywords; omitted, the page keeps its own.`,
                },
                id: { type: "string", description: "A-<n>: the page to replace. Omit to create a new one." },
            },
            required: ["title", "html"],
        },
        call: (args, ctx) => {
            const store = readStore(ctx) ?? new Techdocs(defaultRoot(ctx.cwd));
            return store.publish({
                title: args["title"] as string,
                html: args["html"] as string,
                description: args["description"] as string | undefined,
                keywords: args["keywords"] as string[] | undefined,
                id: args["id"] as string | undefined,
            });
        },
    },
    {
        name: "techdocs_template",
        description:
            "A starting point for a page: every component the viewer styles, in place, with what goes where. Without name, lists the templates.",
        inputSchema: {
            type: "object",
            properties: { name: { type: "string", description: `One of: ${TEMPLATES.map((t) => t.name).join(", ")}.` } },
        },
        call: (args) => {
            if (args["name"] === undefined) {
                return { templates: TEMPLATES.map(({ name, description }) => ({ name, description })) };
            }
            const t = TEMPLATES.find((x) => x.name === args["name"]);
            if (!t) {
                throw new TechdocsError("not_found", `no template "${String(args["name"])}"; there is ${TEMPLATES.map((x) => x.name).join(", ")}`);
            }
            return t;
        },
    },
    {
        name: "techdocs_list",
        description:
            "Every page in the workspace, most recently updated first, with their keywords and without their pages. With keyword, only the pages carrying it.",
        inputSchema: {
            type: "object",
            properties: { keyword: { type: "string", description: "Only the pages carrying this keyword (case does not matter)." } },
        },
        call: (args, ctx) => {
            const keyword = args["keyword"];
            if (keyword !== undefined && typeof keyword !== "string") throw new TechdocsError("invalid", "keyword must be text");
            const all = readStore(ctx)?.list() ?? [];
            const pages = keyword === undefined ? all : all.filter((a) => hasKeyword(a, keyword));
            return { pages, count: pages.length };
        },
    },
    {
        name: "techdocs_get",
        description: "One page's metadata (keywords included) and its HTML.",
        inputSchema: {
            type: "object",
            properties: { id: { type: "string", description: "A-<n>" } },
            required: ["id"],
        },
        call: (args, ctx) => {
            const store = readStore(ctx);
            if (!store) throw new TechdocsError("not_found", `no page ${String(args["id"])}: this workspace has none`);
            return store.get(args["id"] as string);
        },
    },
];

/* ------------------------------------------------------------ JSON-RPC */

function checkArgs(tool: Tool, args: Json): void {
    const props = (tool.inputSchema["properties"] ?? {}) as Json;
    for (const k of Object.keys(args)) {
        if (!(k in props)) {
            throw new TechdocsError("invalid", `${tool.name} takes no "${k}"; it takes ${Object.keys(props).join(", ") || "nothing"}`);
        }
    }
}

export async function handle(msg: Json, ctx: Ctx): Promise<Json | null> {
    const id = msg["id"];
    const method = String(msg["method"] ?? "");
    const reply = (result: unknown) => ({ jsonrpc: "2.0", id, result });
    if (id === undefined || id === null) {
        return null; // a notification: nothing to answer
    }
    switch (method) {
        case "initialize":
            return reply({
                protocolVersion: String((msg["params"] as Json | undefined)?.["protocolVersion"] ?? "2024-11-05"),
                capabilities: { tools: {} },
                serverInfo: { name: "techdocs", version: VERSION },
                instructions: INSTRUCTIONS,
            });
        case "ping":
            return reply({});
        case "tools/list":
            return reply({ tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) });
        case "tools/call": {
            const params = (msg["params"] ?? {}) as Json;
            const tool = TOOLS.find((t) => t.name === params["name"]);
            if (!tool) {
                return { jsonrpc: "2.0", id, error: { code: -32602, message: `unknown tool ${String(params["name"])}` } };
            }
            try {
                const args = (params["arguments"] ?? {}) as Json;
                checkArgs(tool, args);
                const result = await tool.call(args, ctx);
                return reply({ content: [{ type: "text", text: JSON.stringify(result, null, 2) }] });
            } catch (e) {
                const code = e instanceof TechdocsError ? e.code : "internal";
                return reply({ content: [{ type: "text", text: `${code}: ${(e as Error).message}` }], isError: true });
            }
        }
        default:
            return { jsonrpc: "2.0", id, error: { code: -32601, message: `unknown method ${method}` } };
    }
}

export function main(): void {
    const ctx: Ctx = { cwd: process.cwd() };
    const rl = readline.createInterface({ input: process.stdin });
    rl.on("line", (line) => {
        if (!line.trim()) return;
        let msg: Json;
        try {
            msg = JSON.parse(line) as Json;
        } catch {
            process.stdout.write(JSON.stringify({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "parse error" } }) + "\n");
            return;
        }
        void handle(msg, ctx).then((out) => {
            if (out) process.stdout.write(JSON.stringify(out) + "\n");
        });
    });
}
