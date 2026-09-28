# procode

`procode`, short for progressive coding, is a set of (very opinionated) tools 
for working alongside AI coding agents. The agent works through CLIs and MCP servers; 
this extension is where you follow along. It adds one **procode** icon to the activity 
bar, holding four views: **Board**, **Lap History**, **techdocs** and **Knowledge**.

The tools can generally be used with any agent (Humans included too!), but only Claude 
Code is tested.

## Before You Install

**DIY Warning!** This project requires binaries, which are not shipped with the extension, 
but instead must be compiled manually. CMake build & install script are available, check the main
README for exact instructions: [https://github.com/praisethemoon/procode](https://github.com/praisethemoon/procode)

If you do not want all the features, they are also available as individual extensions
on the github repo, and you can build from source and handpick what you like.

Also, these tools and skills, will cause your agent to spend more tokens, on average, it
increases usage by 10 to 15% from my experiments. (`lap` is a CLI so it is hard to measure).

What you get, is order and code that you understand and can reason about. Meant to be the literate 
opposite of vibe coding.

Finally, `procode` is meant to be used only by you. In otherwords, not for collaborative projects. 
`lap` and `coboard` are not meant to be used across multiple parallel branches and forks. 
`lap` has some support for branching however, purely for fanning git worktrees by agents and
merging.

## Coboard
A simple board of epics, milestones and tickets, meant as a more robust
alternative to an agent's plan: you keep track of progress, comment on
tickets, and see what the agent is doing. Agents work it through the
`coboard` MCP server; you work it here. When lap is installed, a ticket also
shows the lap sessions made under it.

![The Board](https://raw.githubusercontent.com/praisethemoon/procode/master/assets/coboard.webp)

## lap & Lap History

`lap` is an edit recorder. Like git it records changes, but every edit an agent
makes is its own commit, with the intent behind it and the behavior it gives
the code, grouped into sessions. Lap History shows those sessions and their
changes, so you can come back later and see what happened and why.

![Lap History](https://raw.githubusercontent.com/praisethemoon/procode/master/assets/lap.webp)

This requires `lap` binary to be installed and available in your PATH

## techdocs

Pages an agent publishes for you to read in the editor: reports, comparisons,
findings, anything with a table or a chart. They are stored in the project's
`.techdocs/`, published through the `techdocs` MCP server, and shown in your
VS Code theme.

![techdocs](https://raw.githubusercontent.com/praisethemoon/procode/master/assets/techdocs.webp)

This is equivalent to Claude's artifact, except they live locally, and vscode theme aware.

## kb & Knowledge (Experimental Feature)

kb is a local knowledge base of documentation, source and papers that agents
and you can both search and cite, with provenance for every passage. Agents
use it through the `kb` MCP server, so research filed once is searched from
disk the next time instead of fetched again. Knowledge browses its
collections and documents, searches them, and shows how documents link to
each other.

![Knowledge](https://raw.githubusercontent.com/praisethemoon/procode/master/assets/kb.webp)

The `kb` cli must be available, and the embedding model msut have already be setup,
more details in the main readme.

## Requirements

Lap History and the Board's sessions use the `lap` CLI, and Knowledge uses
the `kb` CLI. Build both with CMake from the procode repository
([Build and install](https://github.com/praisethemoon/procode#build-and-install)).
procode finds them on PATH, or wherever the settings **Board › Lap Path** and
**Knowledge › Cli Path** point. techdocs needs neither.

procode keeps `lap` and `kb` in `~/.procode/bin`, the path its skills tell
agents to use: copies of the ones it ships, or links to yours when a setting
names them. It refreshes them when VS Code starts, and never deletes
anything there: after uninstalling procode, delete `~/.procode` yourself if
you like.

## Set up Claude Code in a project

VS Code's own agent should get procode's MCP servers without any setup. For Claude
Code, once per project:

1. Open the project's folder in VS Code.
2. Open the Command Palette (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd>, or
   <kbd>Cmd</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> on macOS) and run
   **procode: Set Up MCP for Claude Code**. It adds the `coboard`, `kb` and
   `techdocs` servers to the project's `.mcp.json` and keeps any others there.
3. Run **procode: Add Skills for Claude Code**. It adds the skills that teach
   Claude to use the tools to the project's `.claude/skills/`: pick the ones
   you want (`lap` and `techdocs` are ticked; `tickets` is procode's own
   workflow for board, lap and git together).
4. Start Claude Code in the project, or restart it if it is running, and
   approve the project's MCP servers when it asks.
5. Create the stores that need it: `lap init` and `kb init` in the project's
   root. The board and `.techdocs/` are created on first write.

After you install a new version of procode, it notices when the project's
`.mcp.json` still runs the servers of the old one, and when newer skills
ship; each time it offers to update them. Restart Claude Code afterwards so
it starts the new servers.

## Settings

| Setting | What it is |
|---|---|
| **Board › Lap Path** | The `lap` executable, for the sessions linked to a ticket. |
| **Lap › Path** | The `lap` executable for Lap History. Empty uses **Board › Lap Path**. |
| **Knowledge › Cli Path** | The `kb` executable. |
| **Board › Board Folder** | Another folder's board to work, instead of this folder's. |
| **Board › Author** | The name your comments are signed with. |

## License

MIT. Source, issues and the CLIs:
[github.com/praisethemoon/procode](https://github.com/praisethemoon/procode).

## Have fun.