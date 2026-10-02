# Progressive Coding

Progressive Coding (`procode` for short) is a set of tools for working
alongside AI coding agents in VS Code. It adds one **procode** icon to the activity bar,
holding four views: **Board**, **Lap History**, **techdocs** and **Knowledge**, and it
gives agents three MCP servers (`coboard`, `kb` and `techdocs`) with no setup.
The agent works through those servers; the views are where you follow along.

## What this extension installs and runs

- **Two command-line tools, `lap` and `kb`.** The packages for macOS, Linux and Windows
  include them as native executables. Each time VS Code starts, the extension places
  them in `~/.procode/bin`: copied from the package, or linked when a setting names
  your own build. It only rewrites them when they changed, and never deletes anything
  there; after uninstalling, delete `~/.procode` yourself if you like. The source code
  of these binaries is available in the [GitHub repository](https://github.com/praisethemoon/procode).
- **The views run `lap` and `kb`** to read the project's edit history and knowledge base.
- **Three MCP servers** (Node scripts run by VS Code's own runtime) are registered with
  VS Code; VS Code's agent starts them when it uses them, and the `coboard` and `kb`
  servers run `lap` and `kb` in turn.
- **Files in your project, only when you run a command:** **procode: Set Up MCP for
  Claude Code** writes `.mcp.json`, and **procode: Add Skills for Claude Code** writes
  `.claude/skills/`. When a newer version ships, procode offers to update them.

## Privacy

`procode` collects no data and has no telemetry. Nothing leaves your machine.
The only feature that requires network access is **Knowledge's Add URL**, which will index
the URL you have requested into your local knowledge base.

Everything, in more detail, is on the website:
[praisethemoon.github.io/procode](https://praisethemoon.github.io/procode/).
procode is developed and tested mostly with Claude Code (see
[Set up Claude Code](#set-up-claude-code-in-a-project)); the tools can generally be used
with any agent (humans included too!).

## Before You Install

procode runs two small CLIs, `lap` and `kb`. The packages for macOS, Linux and
Windows include them, so there is nothing else to install. On any other platform
you get the package without them, and build them yourself: CMake build & install
scripts are available, check the main README for exact instructions:
[Build and install](https://github.com/praisethemoon/procode#build-and-install)

If you do not want all the features, hide the views you don't use (right-click a view's
header). Each view is also its own extension in the github repo, if you want to build
from source and handpick what you like; only procode itself sets up the MCP servers
and skills for your agent, though.

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

This uses the `lap` CLI, which procode ships for macOS, Linux and Windows.

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

This uses the `kb` CLI, which procode ships for macOS, Linux and Windows. Keyword
search works as it is; semantic search and reranking also need kb's models,
which you download once: unzip
[kb-models.zip](https://procode.s3.fr-par.scw.cloud/kb-models.zip) (632 MB,
Apache-2.0) into `~/.kb/models/`:

```sh
mkdir -p ~/.kb/models && cd ~/.kb/models
curl -fLO https://procode.s3.fr-par.scw.cloud/kb-models.zip && unzip -o kb-models.zip && rm kb-models.zip
```

The commands for Windows and the checksums are on the
[website](https://praisethemoon.github.io/procode/#kb).

## Requirements

Lap History and the Board's sessions use the `lap` CLI, and Knowledge uses
the `kb` CLI; techdocs needs neither. The packages for macOS, Linux and
Windows include both. Elsewhere, build them with CMake from the procode
repository ([Build and install](https://github.com/praisethemoon/procode#build-and-install)).
procode uses the ones the settings **Board › Lap Path** and **Knowledge ›
Cli Path** name when you set them, else its own, else the ones on PATH.

`~/.procode/bin` (see [What this extension installs and runs](#what-this-extension-installs-and-runs))
is the path procode's skills tell agents to use, so an agent finds the same
`lap` and `kb` the views use.

## Set up Claude Code in a project

VS Code's agent mode gets procode's MCP servers without any setup. Claude Code
reads a project's own files instead, so it needs these steps once per project:

1. Open the project's folder in VS Code.
2. Open the Command Palette (<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd>, or
   <kbd>Cmd</kbd>+<kbd>Shift</kbd>+<kbd>P</kbd> on macOS) and run
   **procode: Set Up MCP for Claude Code**. It adds the `coboard`, `kb` and
   `techdocs` servers to the project's `.mcp.json` and keeps any others there.
   To run only some of them, use **procode: Choose MCP Servers…** (or the
   procode › MCP settings) first: a server that is off is left out.
3. Run **procode: Add Skills for Claude Code**. It adds the skills that teach
   Claude to use the tools to the project's `.claude/skills/`: pick the ones
   you want (`lap`, `kb` and `techdocs` are ticked; `tickets` is procode's own
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