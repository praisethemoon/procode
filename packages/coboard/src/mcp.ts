/* The board as MCP tools, over stdio: newline-delimited JSON-RPC 2.0.
 *
 * Agents find, read, create, update, move and comment on items, and look up
 * the lap sessions linked to a ticket. Deleting is left to people, in the
 * editor.
 *
 * The board is the `.coboard/` found by walking up from the directory the
 * server was started in. With none, reads answer an empty board and the first
 * write creates one at the enclosing git repository's root — never anywhere
 * that is not a repository.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as readline from "node:readline";

import { ticketSessions, sessionCommits, sessionCommand } from "./lap";
import { PRIORITIES, SIZES, TICKET_STATUSES } from "./model";
import { search, view } from "./query";
import { Board, BoardError, CreateInput, Fields, findBoard } from "./store";

const VERSION = "0.1.0";

const INSTRUCTIONS = `coboard is the project's board, shared by developers and agents.
Epics (E-<n>) contain milestones (M-<n>); tickets (T-<n>) always belong to an epic and optionally to one of its milestones.
Always refer to items by these ids. Descriptions and comments are Markdown; mention other items by id (e.g. "blocked by T-4").
Ticket statuses: ${TICKET_STATUSES.join(", ")}. Sizes: ${SIZES.join(", ")}. Priorities: ${PRIORITIES.join(", ")}.
When you work on a ticket, move it to "doing", and record your edits in a lap session linked to it: ${sessionCommand("T-<n>")}.
board_sessions then shows the work done for a ticket.`;

type Json = Record<string, unknown>;

interface Tool {
    readonly name: string;
    readonly description: string;
    readonly inputSchema: Json;
    readonly call: (args: Json, ctx: Ctx) => Promise<unknown> | unknown;
}

interface Ctx {
    readonly cwd: string;
    readonly author: string;
}

const str = { type: "string" };
const FILTERS: Json = {
    kind: { type: "string", enum: ["epic", "milestone", "ticket"] },
    status: str,
    epic: { type: "string", description: "E-<n>: items in this epic" },
    milestone: { type: "string", description: 'M-<n>, or "none" for tickets in no milestone' },
    assignee: str,
    label: str,
};
const FIELDS: Json = {
    title: str,
    description: { type: "string", description: "Markdown" },
    status: str,
    size: { type: ["string", "null"], enum: [...SIZES, null] },
    priority: { type: "string", enum: [...PRIORITIES] },
    assignee: { type: ["string", "null"] },
    labels: { type: "array", items: str },
};

function schema(properties: Json, required: string[] = []): Json {
    return { type: "object", properties, required, additionalProperties: false };
}

/* The board to read: the one above cwd, or an empty one. */
function readBoard(ctx: Ctx): Board | null {
    const root = findBoard(ctx.cwd);
    return root ? new Board(root) : null;
}

/* The board to write: the one above cwd, else a new one at the git root. */
function writeBoard(ctx: Ctx): Board {
    const root = findBoard(ctx.cwd);
    if (root) {
        return new Board(root);
    }
    let dir = path.resolve(ctx.cwd);
    for (;;) {
        if (fs.existsSync(path.join(dir, ".git"))) {
            return new Board(dir);
        }
        const up = path.dirname(dir);
        if (up === dir) {
            throw new BoardError(
                "no_board",
                `no .coboard/ above ${ctx.cwd} and no git repository to create one in; start the server inside the project`,
            );
        }
        dir = up;
    }
}

function items(ctx: Ctx) {
    return readBoard(ctx)?.all() ?? [];
}

function pick<T extends object>(args: Json, keys: readonly string[]): T {
    const out: Json = {};
    for (const k of keys) {
        if (args[k] !== undefined) out[k] = args[k];
    }
    return out as T;
}

const FIELD_KEYS = ["title", "description", "status", "size", "priority", "assignee", "labels"] as const;
const FILTER_KEYS = ["kind", "status", "epic", "milestone", "assignee", "label"] as const;

export const TOOLS: readonly Tool[] = [
    {
        name: "board_list",
        description: "List board items (epics, milestones, tickets) as one-line summaries, optionally filtered. With no filter it lists everything.",
        inputSchema: schema(FILTERS),
        call: (args, ctx) => search(items(ctx), "", pick(args, FILTER_KEYS)),
    },
    {
        name: "board_search",
        description:
            "Search the board. Every word of the query must appear in an item's id, title, description, labels, assignee or comments; an exact id ranks first. Takes the same filters as board_list.",
        inputSchema: schema({ query: str, ...FILTERS, limit: { type: "number" } }, ["query"]),
        call: (args, ctx) => search(items(ctx), String(args["query"] ?? ""), pick(args, [...FILTER_KEYS, "limit"])),
    },
    {
        name: "board_get",
        description:
            "Read one item by id with its context: an epic with its milestones (and progress) and the tickets in no milestone; a milestone with its tickets; a ticket with its description, comments, epic, milestone and linked lap sessions.",
        inputSchema: schema({ id: { type: "string", description: "E-<n>, M-<n> or T-<n>" } }, ["id"]),
        call: async (args, ctx) => {
            const id = String(args["id"] ?? "");
            const v = view(items(ctx), id);
            if (!v) throw new BoardError("not_found", `no ${id.trim().toUpperCase()} on this board`);
            if (v.kind !== "ticket") return v;
            const root = findBoard(ctx.cwd)!;
            const sessions = await ticketSessions(root, v.ticket.id);
            return { ...v, sessions: sessions.value };
        },
    },
    {
        name: "board_create",
        description:
            "Create an epic, a milestone (needs epic) or a ticket (needs epic, or a milestone which implies its epic). Returns the new item with its id.",
        inputSchema: schema(
            { kind: { type: "string", enum: ["epic", "milestone", "ticket"] }, epic: str, milestone: str, ...FIELDS },
            ["kind", "title"],
        ),
        call: (args, ctx) => writeBoard(ctx).create(pick<CreateInput>(args, ["kind", "epic", "milestone", ...FIELD_KEYS])),
    },
    {
        name: "board_update",
        description: "Change fields of an item. Only the fields given change. size, priority, assignee and labels are ticket fields.",
        inputSchema: schema({ id: str, ...FIELDS }, ["id"]),
        call: (args, ctx) => writeBoard(ctx).update(String(args["id"] ?? ""), pick<Fields>(args, FIELD_KEYS)),
    },
    {
        name: "board_move",
        description:
            "Move a ticket to another epic and/or milestone (milestone: null takes it out of its milestone), or a milestone and its tickets to another epic.",
        inputSchema: schema({ id: str, epic: str, milestone: { type: ["string", "null"] } }, ["id"]),
        call: (args, ctx) => writeBoard(ctx).move(String(args["id"] ?? ""), pick(args, ["epic", "milestone"])),
    },
    {
        name: "board_comment",
        description: "Add a Markdown comment to a ticket.",
        inputSchema: schema({ ticket: str, body: str }, ["ticket", "body"]),
        call: (args, ctx) => writeBoard(ctx).comment(String(args["ticket"] ?? ""), String(args["body"] ?? ""), ctx.author),
    },
    {
        name: "board_sessions",
        description: `The lap sessions linked to a ticket, each with its commits: the work done for it. Link a session with: ${sessionCommand("T-<n>")}`,
        inputSchema: schema({ ticket: str }, ["ticket"]),
        call: async (args, ctx) => {
            const board = readBoard(ctx);
            const ticket = String(args["ticket"] ?? "").trim().toUpperCase();
            if (!board) throw new BoardError("not_found", `no ${ticket} on this board`);
            board.get(ticket);
            const s = await ticketSessions(board.root, ticket);
            const sessions = [];
            for (const session of s.value) {
                sessions.push({ ...session, commits: (await sessionCommits(board.root, session.id)).value });
            }
            return { ticket, sessions, ...(s.ok ? {} : { lapError: s.error }), link: sessionCommand(ticket) };
        },
    },
];

/* ------------------------------------------------------------ JSON-RPC */

function checkArgs(tool: Tool, args: Json): void {
    const props = (tool.inputSchema["properties"] ?? {}) as Json;
    for (const k of Object.keys(args)) {
        if (!(k in props)) {
            throw new BoardError("invalid", `${tool.name} takes no "${k}"; it takes ${Object.keys(props).join(", ") || "nothing"}`);
        }
    }
    for (const k of (tool.inputSchema["required"] ?? []) as string[]) {
        if (args[k] === undefined || args[k] === null || args[k] === "") {
            throw new BoardError("invalid", `${tool.name} needs "${k}"`);
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
                serverInfo: { name: "coboard", version: VERSION },
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
                const code = e instanceof BoardError ? e.code : "internal";
                return reply({ content: [{ type: "text", text: `${code}: ${(e as Error).message}` }], isError: true });
            }
        }
        default:
            return { jsonrpc: "2.0", id, error: { code: -32601, message: `unknown method ${method}` } };
    }
}

export function main(): void {
    const ctx: Ctx = {
        cwd: process.cwd(),
        author: process.env["COBOARD_AUTHOR"] || process.env["LAP_USER"] || "agent",
    };
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
