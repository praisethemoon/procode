/* Claude Code's entries in a project's .mcp.json: the servers VS Code's agent
 * runs, with coboard told to sign its comments as Claude. */

import { ServerEntry } from "./mcpjson";

/* Claude Code's comments on the board are signed with this name; without it
 * coboard falls back to "agent", which says nothing about who wrote them. */
export const CLAUDE_AUTHOR = "claude";

export function forClaude(s: ServerEntry): ServerEntry {
    return s.name === "coboard" ? { ...s, env: { ...s.env, COBOARD_AUTHOR: CLAUDE_AUTHOR } } : s;
}
