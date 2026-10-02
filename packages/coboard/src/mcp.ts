/* The board as MCP tools, over stdio: newline-delimited JSON-RPC 2.0.
 *
 * Agents find, read, create, update, move, comment on and archive items, and
 * look up the lap sessions linked to a ticket. Deleting is left to people, in
 * the editor; archiving is not deleting — it is reversible and loses nothing.
 *
 * The board is the `.coboard/` found by walking up from the directory the
 * server was started in. With none, reads answer an empty board and the first
 * write creates one at the enclosing git repository's root — never anywhere
 * that is not a repository.
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as readline from "node:readline";

import { endTicketSession } from "./finish";
import { lapRoot, lapRun, ticketSessions, sessionCommits, sessionCommand } from "./lap";
import { ARCHIVED_MODES, ArchivedMode, PRIORITIES, SIZES, TICKET_STATUSES } from "./model";
import { search, view } from "./query";
import { Board, BoardError, CreateInput, Fields, locateBoard, staleParentMessage } from "./store";

const VERSION = "0.1.1";

const INSTRUCTIONS = `coboard is the project's board, shared by developers and agents.
Epics (E-<n>) contain milestones (M-<n>); tickets (T-<n>) always belong to an epic and optionally to one of its milestones.
Always refer to items by these ids. Descriptions and comments are Markdown; mention other items by id (e.g. "blocked by T-4").
Ticket statuses: ${TICKET_STATUSES.join(", ")}. Sizes: ${SIZES.join(", ")}. Priorities: ${PRIORITIES.join(", ")}.
When you work on a ticket, move it to "doing", and record your edits in a lap session linked to it: ${sessionCommand("T-<n>")}.
board_sessions then shows the work done for a ticket.
When the work is done, ticket_finish closes it in one step: it ends the session with its summary, comments, sets the status, and hands back the git command that commits exactly the session's files, for you to run.
Finished work can be archived (board_archive) to keep lists short: an archived epic or milestone takes everything under it, lists and search leave archived items out unless asked (archived: "include" or "only"), board_get still reads them, and board_unarchive brings them back. Archive only what is finished or abandoned, and say why. Never delete.`;

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
    archived: {
        type: "string",
        enum: [...ARCHIVED_MODES],
        description: 'Archived items: "exclude" (the default) leaves them out, "include" adds them, "only" lists just them',
    },
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

/* Where the board is, refusing a lap branch whose parent is gone: its own
 * copy of the board would be read, and written, as if it were the board. */
function located(ctx: Ctx): ReturnType<typeof locateBoard> {
    const at = locateBoard(ctx.cwd);
    if (at.via === "stale-parent") {
        throw new BoardError("stale_parent", staleParentMessage(at.stale!));
    }
    return at;
}

/* The board to read: the project's (locateBoard), or an empty one. */
function readBoard(ctx: Ctx): Board | null {
    const root = located(ctx).root;
    return root ? new Board(root) : null;
}

/* The board to write: the project's (locateBoard), else a new one in a lap
 * branch's parent, else at the git root. */
function writeBoard(ctx: Ctx): Board {
    const at = located(ctx);
    if (at.root) {
        return new Board(at.root);
    }
    if (at.home) {
        return new Board(at.home);
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

/* Every item, archived or not: each tool applies its own `archived` filter,
 * and an archived item can still be read by id. */
function items(ctx: Ctx) {
    return readBoard(ctx)?.all({ archived: "include" }) ?? [];
}

function pick<T extends object>(args: Json, keys: readonly string[]): T {
    const out: Json = {};
    for (const k of keys) {
        if (args[k] !== undefined) out[k] = args[k];
    }
    return out as T;
}

const FIELD_KEYS = ["title", "description", "status", "size", "priority", "assignee", "labels"] as const;
const FILTER_KEYS = ["kind", "status", "epic", "milestone", "assignee", "label", "archived"] as const;

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
            "Read one item by id with its context: an epic with its milestones (and progress) and the tickets in no milestone; a milestone with its tickets; a ticket with its description, comments, epic, milestone and linked lap sessions. Archived items are read too, and say so (archived: at, via, by, reason); an epic's or milestone's archived contents are left out unless archived is \"include\".",
        inputSchema: schema({ id: { type: "string", description: "E-<n>, M-<n> or T-<n>" }, archived: FILTERS["archived"] as Json }, ["id"]),
        call: async (args, ctx) => {
            const id = String(args["id"] ?? "");
            const v = view(items(ctx), id, { archived: (args["archived"] as ArchivedMode | undefined) ?? "exclude" });
            if (!v) throw new BoardError("not_found", `no ${id.trim().toUpperCase()} on this board`);
            if (v.kind !== "ticket") return v;
            const root = located(ctx).root!;
            const sessions = await ticketSessions(root, v.ticket.id);
            /* no sessions because lap failed is not "no work": say so */
            return { ...v, sessions: sessions.value, ...(sessions.ok ? {} : { lapError: sessions.error }) };
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
        name: "board_archive",
        description:
            "Archive an item: finished or abandoned work, out of lists and search. Archiving an epic or milestone archives everything under it. Reversible with board_unarchive; nothing is lost. Give the reason.",
        inputSchema: schema({ id: str, reason: { type: "string", description: "Why, in a sentence" } }, ["id"]),
        call: (args, ctx) =>
            writeBoard(ctx).archive(String(args["id"] ?? ""), {
                by: ctx.author,
                ...(args["reason"] !== undefined ? { reason: String(args["reason"]) } : {}),
            }),
    },
    {
        name: "board_unarchive",
        description:
            "Bring back an archived item, and with it everything under it. An item archived with its epic or milestone comes back when that is unarchived.",
        inputSchema: schema({ id: str }, ["id"]),
        call: (args, ctx) => writeBoard(ctx).unarchive(String(args["id"] ?? "")),
    },
    {
        name: "board_sessions",
        description: `The lap sessions linked to a ticket, each with its commits: the work done for it. A commit gives its intent (why the edit exists; edits serving one goal share it), its behavior (what the edit makes the code do), its hash, forced: true when its author bypassed lap's message checks, and amended: <n> when lap amend corrected its intent and behavior (the text given is the latest). Link a session with: ${sessionCommand("T-<n>")}`,
        inputSchema: schema({ ticket: str }, ["ticket"]),
        call: async (args, ctx) => {
            const board = readBoard(ctx);
            const ticket = String(args["ticket"] ?? "").trim().toUpperCase();
            if (!board) throw new BoardError("not_found", `no ${ticket} on this board`);
            board.get(ticket);
            const s = await ticketSessions(board.root, ticket);
            const sessions = [];
            const errors = s.ok ? [] : [s.error ?? "lap failed"];
            for (const session of s.value) {
                /* a session still only in a branch: that branch's, as ids repeat across folders */
                const c = await sessionCommits(board.root, session.id, session.branch);
                if (!c.ok) errors.push(`session ${session.branch ? `${session.branch}/` : ""}${session.id}: ${c.error ?? "lap failed"}`);
                sessions.push({ ...session, commits: c.value });
            }
            return { ticket, sessions, ...(errors.length ? { lapError: errors.join("; ") } : {}), link: sessionCommand(ticket) };
        },
    },
    {
        name: "ticket_finish",
        description:
            "Finish a ticket in one step: end its active lap session with a summary, comment on the ticket (session, summary, tests, what was not verified), set its status, and return the git command that commits exactly the files the session touched plus lap's and the board's logs. It never runs git: run the command (or adjust it) yourself. Refused, changing nothing, when the ticket has no active session or a file the session touched has edits lap has not recorded.",
        inputSchema: schema(
            {
                ticket: str,
                done: { type: "string", description: "What was done: the session summary's Done, and the comment's." },
                decided: { type: "string", description: "What was decided and why (optional)." },
                left: { type: "string", description: "What was left undone (optional)." },
                subject: { type: "string", description: "The git commit's subject; the ticket id is added at the end." },
                tests: { type: "string", description: "What was tested, with counts (optional, for the comment)." },
                not_verified: { type: "string", description: "What was not verified (optional, for the comment)." },
                status: { type: "string", enum: ["done", "review"], description: 'The ticket\'s status after: "done" (default) or "review".' },
            },
            ["ticket", "done", "subject"],
        ),
        call: async (args, ctx) => {
            const board = writeBoard(ctx);
            const ticket = String(args["ticket"] ?? "").trim().toUpperCase();
            const item = board.get(ticket);
            if (item.kind !== "ticket") throw new BoardError("invalid", `${ticket} is not a ticket`);
            const status = String(args["status"] ?? "done");
            if (status !== "done" && status !== "review") throw new BoardError("invalid", `status must be done or review, not "${status}"`);
            const root = lapRoot(ctx.cwd);
            if (!root) throw new BoardError("no_lap", `no lap repository at or above ${ctx.cwd}: nothing records ${ticket}'s work`);
            const opt = (k: string) => (typeof args[k] === "string" ? (args[k] as string) : undefined);
            const ended = await endTicketSession(
                lapRun(ctx.cwd),
                {
                    ticket,
                    done: String(args["done"]),
                    decided: opt("decided"),
                    left: opt("left"),
                    subject: String(args["subject"]),
                    tests: opt("tests"),
                    notVerified: opt("not_verified"),
                },
                path.resolve(board.root) === root,
            );
            // the session has ended: a failure from here says what is done
            try {
                board.comment(ticket, ended.comment, ctx.author);
                board.update(ticket, { status });
            } catch (e) {
                throw new BoardError(
                    "partly_done",
                    `${ended.session} ended, but the board could not be written (${(e as Error).message}): comment and set ${ticket}'s status by hand; git command: ${ended.git.command}`,
                );
            }
            return {
                ticket,
                session: ended.session,
                status,
                git: { cwd: root, paths: ended.git.paths, message: ended.git.message, command: ended.git.command },
                ...(ended.otherPending.length ? { otherPending: ended.otherPending } : {}),
                next: `Review and run the git command in ${root}; it stages only the files ${ended.session} touched, and lap's and the board's logs.`,
            };
        },
    },
];

/* ------------------------------------------------------------ JSON-RPC */

function checkArgs(tool: Tool, args: Json): void {
    const mode = args["archived"];
    if (mode !== undefined && !ARCHIVED_MODES.includes(mode as ArchivedMode)) {
        throw new BoardError("invalid", `archived "${String(mode)}" is not one of ${ARCHIVED_MODES.join(", ")}`);
    }
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
