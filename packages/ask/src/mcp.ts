/* ask as MCP tools: JSON-RPC 2.0 messages in, replies out (specs/ask.md §4).
 * The transport is http.ts; this file knows nothing of it.
 *
 * `ask` opens a form in the person's editor and answers when they submit
 * it. The call waits, sending progress notifications so the client knows it
 * is alive, and gives up after WAIT_MS with the form still open; `ask_wait`
 * picks the same form up again. A client that cancels the call cancels the
 * form, and the editor tab closes.
 */

import { AskError, MAX_TITLE, Request, StepAnswer, parseSteps, results } from "./form";
import { Forms, Wait, isFormId } from "./forms";

const VERSION = "0.1.0";

/** How long one call waits before handing back a form still open. Progress
 *  keeps the call alive past Claude Code's idle limit; this bounds a call
 *  from a client that sends no progress token, and one left open for good. */
export const WAIT_MS = 25 * 60_000;
const PROGRESS_MS = 15_000;

export const INSTRUCTIONS = `ask opens a form in the person's VS Code: one question per page, which they answer in any order, skip, or send back asking for more. Use it instead of asking in chat, and instead of your own question tool, when you have two or more questions, or one that needs a preview to choose between.

Each step is one question: a title, a Markdown body with the context the person needs, and a kind — single (pick one), multi (pick any) or text (write). Choice steps get a free-text "Other" unless you turn it off. An option may carry an HTML preview, shown sandboxed beside the options: style it with baukasten's --bk-* variables, as techdocs pages are, and inline everything.

The call waits until the person submits. Each step comes back answered, skipped, or needs_more with what the person wants to know. For needs_more steps, put the answer to their question in the step's body and ask those steps again with from set to the form's id: what they already answered is filled in.`;

type Json = Record<string, unknown>;

export interface Ctx {
    /** The window's forms. */
    readonly forms: Forms;
    /** Sends a notification to the client (progress, here). */
    readonly notify?: (msg: Json) => void;
    /** Aborted when the client cancels the call or goes away. */
    readonly signal?: AbortSignal;
    /** The call's progress token, when the client sent one. */
    readonly progressToken?: string | number;
    /** WAIT_MS and PROGRESS_MS, shorter in tests. */
    readonly waitMs?: number;
    readonly progressMs?: number;
}

interface Tool {
    readonly name: string;
    readonly description: string;
    readonly inputSchema: Json;
    readonly call: (args: Json, ctx: Ctx) => unknown;
}

/** The answers of an earlier form to fill in again: those the person gave,
 *  for the steps asked again. Skipped and needs_more steps start open. */
function carried(forms: Forms, from: unknown, stepIds: readonly string[]): Record<string, StepAnswer> | undefined {
    if (from === undefined) return undefined;
    if (!isFormId(from)) throw new AskError("bad_id", `from must be a form id (F-<n>), not ${JSON.stringify(from)}`);
    const before = forms.answer(from);
    if (!before) throw new AskError("not_found", `form ${from} has no answer to carry over in this window`);
    const out: Record<string, StepAnswer> = {};
    for (const id of stepIds) if (before.steps[id]?.state === "answered") out[id] = before.steps[id];
    return out;
}

async function waitAndReport(req: Request, ctx: Ctx): Promise<Json> {
    const w: Wait = await ctx.forms.wait(req.id, {
        timeoutMs: ctx.waitMs ?? WAIT_MS,
        tickMs: ctx.progressMs ?? PROGRESS_MS,
        signal: ctx.signal,
        onTick: (waited) => {
            if (ctx.progressToken === undefined || !ctx.notify) return;
            ctx.notify({
                jsonrpc: "2.0",
                method: "notifications/progress",
                params: { progressToken: ctx.progressToken, progress: Math.floor(waited / 1000), message: `waiting for the person to answer ${req.id}` },
            });
        },
    });
    switch (w.status) {
        case "submitted": {
            const steps = results(req, w.answer);
            const more = steps.filter((s) => s.state === "needs_more").map((s) => s.id);
            return {
                id: req.id,
                status: "submitted",
                steps,
                ...(more.length
                    ? {
                          next: `The person wants more on step ${more.join(", ")}. Answer their question in each step's body and ask those steps again with from: "${req.id}".`,
                      }
                    : {}),
            };
        }
        case "cancelled":
            return { id: req.id, status: "cancelled", next: "The person closed the form without submitting it." };
        case "waiting":
            return { id: req.id, status: "waiting", next: `The form is still open. Call ask_wait with id "${req.id}" to keep waiting.` };
        case "aborted":
            ctx.forms.cancel(req.id);
            return { id: req.id, status: "cancelled" };
    }
}

export const TOOLS: readonly Tool[] = [
    {
        name: "ask",
        description:
            "Ask the person questions in a form in their VS Code, one question per page, and wait for the answers. Each step returns answered, skipped or needs_more (with what they want to know).",
        inputSchema: {
            type: "object",
            properties: {
                title: { type: "string", description: `What the questions are about, one line, at most ${MAX_TITLE} characters.` },
                steps: {
                    type: "array",
                    description: "The questions, in the order shown; the person may answer them in any order.",
                    items: {
                        type: "object",
                        properties: {
                            id: { type: "string", description: "Stable id, to match answers and to ask again with from. Defaults to the step's position." },
                            title: { type: "string", description: "The question, one line." },
                            body: { type: "string", description: "Markdown: the context needed to answer." },
                            kind: { type: "string", enum: ["single", "multi", "text"], description: "Default: single with options, text without." },
                            options: {
                                type: "array",
                                description: "For single and multi: labels, or {label, description (Markdown), preview (HTML)}.",
                                items: {
                                    anyOf: [
                                        { type: "string" },
                                        {
                                            type: "object",
                                            properties: { label: { type: "string" }, description: { type: "string" }, preview: { type: "string" } },
                                            required: ["label"],
                                        },
                                    ],
                                },
                            },
                            other: { type: "boolean", description: "Offer a free-text Other on a choice step. Default true." },
                        },
                        required: ["title"],
                    },
                },
                from: { type: "string", description: "F-<n>: an earlier form whose answers fill in the steps asked again (matched by step id)." },
            },
            required: ["title", "steps"],
        },
        call: async (args, ctx) => {
            const title = args["title"];
            if (typeof title !== "string" || !title.trim()) throw new AskError("invalid", "title is required");
            if (title.length > MAX_TITLE) throw new AskError("invalid", `title is longer than ${MAX_TITLE} characters`);
            const steps = parseSteps(args["steps"]);
            const previous = carried(
                ctx.forms,
                args["from"],
                steps.map((s) => s.id),
            );
            return waitAndReport(ctx.forms.create(title.trim(), steps, previous), ctx);
        },
    },
    {
        name: "ask_wait",
        description: "Keep waiting for a form that ask handed back still open, and return its answers.",
        inputSchema: {
            type: "object",
            properties: { id: { type: "string", description: "F-<n>" } },
            required: ["id"],
        },
        call: async (args, ctx) => waitAndReport(ctx.forms.request(String(args["id"])), ctx),
    },
];

/* ------------------------------------------------------------ JSON-RPC */

function checkArgs(tool: Tool, args: Json): void {
    const props = (tool.inputSchema["properties"] ?? {}) as Json;
    for (const k of Object.keys(args)) {
        if (!(k in props)) throw new AskError("invalid", `${tool.name} takes no "${k}"; it takes ${Object.keys(props).join(", ")}`);
    }
}

export async function handle(msg: Json, ctx: Ctx): Promise<Json | null> {
    const id = msg["id"];
    const method = String(msg["method"] ?? "");
    const reply = (result: unknown) => ({ jsonrpc: "2.0", id, result });
    if (id === undefined || id === null) return null; // a notification: nothing to answer
    switch (method) {
        case "initialize":
            return reply({
                protocolVersion: String((msg["params"] as Json | undefined)?.["protocolVersion"] ?? "2024-11-05"),
                capabilities: { tools: {} },
                serverInfo: { name: "ask", version: VERSION },
                instructions: INSTRUCTIONS,
            });
        case "ping":
            return reply({});
        case "tools/list":
            return reply({ tools: TOOLS.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) });
        case "tools/call": {
            const params = (msg["params"] ?? {}) as Json;
            const tool = TOOLS.find((t) => t.name === params["name"]);
            if (!tool) return { jsonrpc: "2.0", id, error: { code: -32602, message: `unknown tool ${String(params["name"])}` } };
            const token = ((params["_meta"] ?? {}) as Json)["progressToken"];
            const callCtx: Ctx = typeof token === "string" || typeof token === "number" ? { ...ctx, progressToken: token } : ctx;
            try {
                const args = (params["arguments"] ?? {}) as Json;
                checkArgs(tool, args);
                const result = await tool.call(args, callCtx);
                return reply({ content: [{ type: "text", text: JSON.stringify(result, null, 2) }] });
            } catch (e) {
                const code = e instanceof AskError ? e.code : "internal";
                return reply({ content: [{ type: "text", text: `${code}: ${(e as Error).message}` }], isError: true });
            }
        }
        default:
            return { jsonrpc: "2.0", id, error: { code: -32601, message: `unknown method ${method}` } };
    }
}
