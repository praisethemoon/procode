# Changelog

## 0.3.0 — 2026-10-15

### Extension

- Codex is set up like Claude Code. **procode: Set Up MCP** and **procode: Add Skills** first ask which agent: Claude Code (`.mcp.json`, `.claude/skills/`) or Codex (`.codex/config.toml`, `.agents/skills/`). In `.codex/config.toml` only procode's own `[mcp_servers.…]` tables change; every other line stays as it was. Codex reads that file only in a project it trusts.
- The commands are renamed to match: `procode.setUpClaudeMcp` is now `procode.setUpMcp`, and `procode.addClaudeSkills` is now `procode.addSkills`. A keybinding on an old id needs updating.
- Skills update notices name the agent and folder they are about.

### lap

- Deleting a file no longer trips the message checks: a delete's behavior may repeat the previous one, so deleting many files for one reason needs no `--force-message`. `-b` is optional for a delete (lap writes "Deletes <file>."). The intent is still required.

## 0.2.0 — 2026-10-03

### Extension

- procode's icon is now lap's spiral: in the activity bar, the Extensions view, the README and the website.
- Each MCP server can be turned on or off, under the procode › MCP settings or with **procode: Choose MCP Servers…**. A server that is off is left out of VS Code's agent, and **procode: Set Up MCP for Claude Code** takes out the `.mcp.json` entry it wrote for it. Entries you added by hand are never touched.

### Lap History

- A badge on procode's icon counts the files with edits lap has not recorded yet, as git's does. After VS Code starts, it appears once the procode sidebar has been opened.
- An untrack (below) shows with its own icon, and the Change filter can pick it.

### lap

- `lap untrack <path>…` stops tracking files lap recorded, such as ones `.lapignore` names now, without starting a new history. A folder means every tracked file in it. The files stay on disk and their history stays readable. A file with edits lap has not recorded is refused unless `--force`.
- `lap status` lists tracked files that `.lapignore` now names, with the command that stops tracking them.
- A history with untracks needs this lap: an older one keeps those files tracked.

### Board

- `ticket_finish` (MCP) closes a ticket in one step: it ends the ticket's lap session with a summary, comments on the ticket, sets its status, and hands back the git command that commits exactly the files the session touched. It never runs git.

### Knowledge

- **Index This Workspace** files the open folders, each into its own collection, from the Command Palette or the view's toolbar.
- A document tab has a toolbar: the file name, find with highlighting, zoom from 20% to 400%, and Save As.
- `kb_add` (MCP) files web pages by URL, fetched and filed as they are served.
- kb's models are one download, `kb-models.zip`, unzipped into `~/.kb/models`.

### Skills

- The lap skill explains `lap untrack`, and the tickets skill closes tickets with `ticket_finish`. Projects that added them with **procode: Add Skills for Claude Code** are offered the update.

## 0.1.1 — 2026-09-30

### Extension

- The extension is called Progressive Coding in the VS Code Marketplace and Open VSX. Its id stays `praisethemoon.procode-tools`, so 0.1.0 updates in place.

### Board

- Links inside a board tab now open in that tab, like a browser, instead of a new tab each time. ⌘-click (Ctrl-click on Windows and Linux), middle-click, or ⌘/Ctrl+Enter on a Kanban card opens a new tab. An item that already has a tab is focused instead.
- Each board tab has its own back and forward history, on VS Code's Go Back and Go Forward keys (⌃- and ⌃⇧- on macOS, Alt+← and Alt+→ on Windows, Ctrl+Alt+- and Ctrl+Shift+- on Linux) and on the mouse's back and forward buttons.

## 0.1.0 — 2026-09-29

First release.
