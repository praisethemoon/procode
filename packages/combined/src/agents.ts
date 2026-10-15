/* The coding agents procode sets up in a project, and where each keeps its
 * MCP servers and skills. Each signs its board comments with its own name;
 * without one, coboard falls back to "agent", which says nothing about who
 * wrote them. */

import { ServerEntry } from "./mcpjson";

export type AgentId = "claude" | "codex";

export interface Agent {
    readonly id: AgentId;
    readonly label: string;
    /* the MCP file, from the project's root, and its format */
    readonly mcpFile: readonly string[];
    readonly format: "json" | "toml";
    /* the project's skills folder, from its root */
    readonly skillsDir: readonly string[];
    /* coboard's COBOARD_AUTHOR for this agent's comments */
    readonly author: string;
    /* how the agent picks up a change */
    readonly reload: string;
    /* what the MCP file needs before the agent reads it */
    readonly mcpNote?: string;
}

export const AGENTS: readonly Agent[] = [
    {
        id: "claude",
        label: "Claude Code",
        mcpFile: [".mcp.json"],
        format: "json",
        skillsDir: [".claude", "skills"],
        author: "claude",
        reload: "Restart Claude Code in this project (or check /mcp, /skills)",
    },
    {
        id: "codex",
        label: "Codex",
        mcpFile: [".codex", "config.toml"],
        format: "toml",
        skillsDir: [".agents", "skills"],
        author: "codex",
        reload: "Restart Codex in this project (or check /mcp)",
        mcpNote: "Codex reads .codex/config.toml only in a project it trusts",
    },
];

export function agent(id: AgentId): Agent {
    return AGENTS.find((a) => a.id === id)!;
}

/* The agent's entry for one of our servers: coboard signs as the agent. */
export function forAgent(s: ServerEntry, a: Agent): ServerEntry {
    return s.name === "coboard" && "env" in s ? { ...s, env: { ...s.env, COBOARD_AUTHOR: a.author } } : s;
}
