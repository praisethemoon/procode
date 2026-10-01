import type { ReactNode } from "react";
import { Alert, Code, Heading, Image, Link, Paragraph, Table } from "baukasten-ui/core";
import { Accordion, AccordionItem } from "baukasten-ui/extra";

import coboardShot from "../../assets/coboard.webp";
import kbShot from "../../assets/kb.webp";
import lapShot from "../../assets/lap.webp";
import techdocsShot from "../../assets/techdocs.webp";

const REPO = "https://github.com/praisethemoon/procode";
const BLOB = `${REPO}/blob/master`;

export interface Section {
    readonly id: string;
    /* what the sidebar says */
    readonly title: string;
    /* what the section's own heading says, when it is longer */
    readonly heading?: string;
    readonly body: ReactNode;
}

/* A shell session or a file, written in the source with a leading newline
 * so it lines up; the newline and trailing space are not part of it. */
function Block({ children }: { children: string }) {
    return (
        <Code block wrap={false} className="block">
            {children.replace(/^\n/, "").replace(/\s+$/, "")}
        </Code>
    );
}

const ALERT = { info: "info", warn: "warning", danger: "error" } as const;

function Callout({ kind, title, children }: { kind: keyof typeof ALERT; title: string; children: ReactNode }) {
    return (
        <Alert variant={ALERT[kind]} title={title} className="callout">
            {children}
        </Alert>
    );
}

function Shot({ src, alt, caption }: { src: string; alt: string; caption: string }) {
    return <Image src={src} alt={alt} caption={caption} radius="md" bordered loading="lazy" className="shot" />;
}

function Grid({ head, rows }: { head: readonly string[]; rows: readonly (readonly ReactNode[])[] }) {
    return (
        <div className="grid">
            <Table size="sm">
                <Table.Head>
                    <Table.Row hoverable={false}>
                        {head.map((h) => (
                            <Table.HeaderCell key={h}>{h}</Table.HeaderCell>
                        ))}
                    </Table.Row>
                </Table.Head>
                <Table.Body>
                    {rows.map((r, i) => (
                        <Table.Row key={i}>
                            {r.map((c, j) => (
                                <Table.Cell key={j}>{c}</Table.Cell>
                            ))}
                        </Table.Row>
                    ))}
                </Table.Body>
            </Table>
        </div>
    );
}

function Faq({ q, children }: { q: string; children: ReactNode }) {
    return <AccordionItem title={q}>{children}</AccordionItem>;
}

const intro: Section = {
    id: "intro",
    title: "Introduction",
    heading: "procode",
    body: (
        <>
            <Paragraph size="lg">
                <strong>procode</strong>, short for <em>progressive code</em>, is a set of (very opinionated) tools for
                working alongside AI coding agents. The agent works through small CLIs and MCP servers; a VS Code
                extension, also called procode, is where you follow along.
            </Paragraph>
            <Paragraph>There are four pieces, and each can be used without the others:</Paragraph>
            <ul>
                <li>
                    <Link href="#lap">lap</Link>: an edit recorder. Every edit the agent makes is its own commit, with why it
                    was made and what it does.
                </li>
                <li>
                    <Link href="#kb">kb</Link>: a local knowledge base of documentation, source and papers, so research is
                    filed once and searched from disk afterwards.
                </li>
                <li>
                    <Link href="#coboard">coboard</Link>: a small board of epics, milestones and tickets that you and the agent
                    share, in place of an agent's throwaway plan.
                </li>
                <li>
                    <Link href="#techdocs">techdocs</Link>: reports and findings the agent publishes as pages you read in the
                    editor, in your theme.
                </li>
            </ul>
            <Paragraph>
                What you get is order, and code you understand and can reason about. It is meant as the literate
                opposite of vibe coding.
            </Paragraph>

            <Callout kind="info" title="Made for one person">
                <Paragraph>
                    procode is meant to be used by you, on your own projects. It is not built for large collaborative
                    projects. lap's history and the board are append-only logs committed to git, and two people (or two
                    git branches of one folder) writing to them will conflict in ways that cannot be merged. lap does
                    support branches, but only so that agents can fan out into git worktrees and merge back.
                </Paragraph>
            </Callout>

            <Paragraph>
                The tools work with any agent that can run a command or talk to an MCP server (a human can use them
                too), but only Claude Code is tested. They also cost tokens: in my experiments, about 10 to 15% more
                on average.
            </Paragraph>

            <Heading level={3}>Getting started</Heading>
            <ol>
                <li>
                    Download the <Code>.vsix</Code> for your platform from{" "}
                    <Link href={`${REPO}/releases`}>Releases</Link> and install it with{" "}
                    <strong>Extensions: Install from VSIX…</strong> in VS Code. The packages for macOS, Linux (Alpine included) and
                    Windows include the <Code>lap</Code> and <Code>kb</Code> CLIs, so there is nothing else to install.
                </li>
                <li>
                    Open your project's folder in VS Code and run <strong>procode: Set Up MCP for Claude Code</strong>{" "}
                    from the Command Palette. It adds the <Code>coboard</Code>, <Code>kb</Code> and{" "}
                    <Code>techdocs</Code> servers to the project's <Code>.mcp.json</Code> and keeps any others there.
                </li>
                <li>
                    Run <strong>procode: Add Skills for Claude Code</strong> and pick the skills that teach Claude to use
                    the tools: <Code>lap</Code>, <Code>kb</Code> and <Code>techdocs</Code> are ticked;{" "}
                    <Code>tickets</Code> is procode's own workflow for the board, lap and git together.
                </li>
                <li>Start or restart Claude Code in the project, and approve the project's MCP servers when it asks.</li>
                <li>
                    Run <Code>lap init</Code> and <Code>kb init</Code> in the project root. The board and{" "}
                    <Code>.techdocs/</Code> are created on first write.
                </li>
            </ol>
            <Paragraph>
                VS Code's own agent gets the MCP servers without any setup. When you install a newer procode, it
                notices an outdated <Code>.mcp.json</Code> or older skills and offers to update them.
            </Paragraph>
        </>
    ),
};

const lap: Section = {
    id: "lap",
    title: "lap",
    heading: "lap, the edit recorder",
    body: (
        <>
            <Paragraph size="lg">
                lap records code changes the way agents make them: one small edit at a time, each with the reason for
                it and what it does. It sits below git and never touches it.
            </Paragraph>
            <Paragraph>
                git answers <em>what changed between two commits</em>. lap answers <em>why does this line exist</em>{" "}
                and <em>what was the agent doing when it appeared</em>, at the size agents actually work at.
            </Paragraph>
            <Block>{`
$ lap session start "add retry logic to the fetcher"
session S4 started: add retry logic to the fetcher

$ lap commit src/fetch.c -i "survive the flaky staging DNS, which drops ~2% of lookups" \\
    -b "adds retry(): three attempts with exponential backoff"
[L23 fa9cebd] S4 src/fetch.c: lines 10-24 (insertion)

$ lap search --file src/fetch.c --line 12
line 12 of src/fetch.c was last touched by L23 fa9cebd
session: S4
intent:
  survive the flaky staging DNS, which drops ~2% of lookups
behavior:
  adds retry(): three attempts with exponential backoff
`}</Block>

            <Heading level={3}>How it works</Heading>
            <ul>
                <li>
                    <strong>One commit is one edit</strong>: a contiguous run of changed lines in one file. If a file has
                    two separate changes, <Code>lap commit</Code> refuses and lists them so they can be committed one
                    at a time (<Code>--edit N</Code> or <Code>--lines A-B</Code>). A new file is committed in parts the
                    same way, so a large one never lands as one unexplained blob.
                </li>
                <li>
                    <strong>Every commit explains itself twice.</strong> Its <em>intent</em> (<Code>-i</Code>) says why
                    the edit exists; its <em>behavior</em> (<Code>-b</Code>) says what it makes the code do. lap refuses
                    a message that is too short or only repeats the intent or the code. A commit can cite another by its
                    hash, <Code>#fa9cebd</Code>.
                </li>
                <li>
                    <strong>Work happens in sessions.</strong> <Code>lap session start "purpose"</Code> groups the
                    commits of one task; <Code>lap session end</Code> can record what was done, what was decided and
                    what was left.
                </li>
                <li>
                    <strong>lap never writes to your files.</strong> There is no checkout, revert or staging area. It
                    records; it does not time-travel.
                </li>
            </ul>
            <Paragraph>
                The history is an append-only log in <Code>.lap/log/</Code>, every record hash-chained to the one
                before it, so tampering and corruption show up in <Code>lap verify</Code>. That log is the only truth
                and is committed to git (<Code>lap init</Code> writes a <Code>.lap/.gitignore</Code> that keeps
                everything else out). The rest of <Code>.lap/</Code> is a cache that <Code>lap rebuild</Code> can
                recreate. lap is written in C11 with no dependencies.
            </Paragraph>
            <Paragraph>
                Parallel agents each get a folder of their own (usually a git worktree) made a <strong>lap branch</strong>{" "}
                with <Code>lap branch start</Code>. To bring the work back: <Code>git merge</Code> brings the code and
                the branch's history, then <Code>lap merge</Code> adopts that history.
            </Paragraph>

            <Heading level={3}>How it ships with the extension</Heading>
            <Paragraph>
                The platform packages of the extension (<Code>darwin-arm64</Code>, <Code>darwin-x64</Code>,{" "}
                <Code>linux-x64</Code>, <Code>linux-arm64</Code>, <Code>alpine-*</Code>, <Code>win32-x64</Code>,{" "}
                <Code>win32-arm64</Code>) carry a <Code>lap</Code> and a <Code>kb</Code> binary built for that
                platform. When VS Code starts, procode puts them in <Code>~/.procode/bin</Code>, which is the path its
                skills tell agents to use. If you point the settings <strong>Board › Lap Path</strong> or{" "}
                <strong>Knowledge › Cli Path</strong> at your own build (or have one on <Code>PATH</Code>),{" "}
                <Code>~/.procode/bin</Code> links to that instead.
            </Paragraph>
            <Paragraph>
                <Code>procode-&lt;version&gt;.vsix</Code>, the package without a platform, has no CLIs. Build them
                with CMake, as in the <Link href={`${REPO}#build-and-install`}>README</Link>:
            </Paragraph>
            <Block>{`
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
sudo cmake --install build
`}</Block>
            <Paragraph>
                More in <Link href={`${BLOB}/cli/lap-cli/README.md`}>lap's README</Link> and its{" "}
                <Link href={`${BLOB}/cli/lap-cli/SPEC.md`}>SPEC</Link>.
            </Paragraph>
        </>
    ),
};

const kb: Section = {
    id: "kb",
    title: "kb",
    heading: "kb, the knowledge base",
    body: (
        <>
            <Callout kind="warn" title="Work in progress, experimental">
                <Paragraph>
                    kb is the youngest part of procode. Its commands, its store format and its search are still
                    changing, and a store made by one version may need <Code>kb rebuild</Code> or{" "}
                    <Code>kb reindex</Code> under the next.
                </Paragraph>
            </Callout>
            <Paragraph size="lg">
                kb is a local knowledge base of documentation, source and papers that agents and you can both search
                and cite. Nothing leaves your machine.
            </Paragraph>
            <Paragraph>
                The point is that research is not thrown away. An agent that has read the docs of a library files them
                with <Code>kb_add</Code>; the next time the question comes up, it searches the store on disk instead of
                fetching the web again. Every passage keeps its provenance: where it came from and when it was
                fetched, so old documentation can be spotted (<Code>kb stale</Code>) instead of believed.
            </Paragraph>

            <Heading level={3}>How it works</Heading>
            <ul>
                <li>
                    <strong>One store per workspace</strong>, in <Code>.kb/</Code>, found by walking up from the current
                    folder like <Code>.git</Code>. There is no global store.
                </li>
                <li>
                    <strong>Collections</strong> keep topics apart (<Code>io-uring</Code>, <Code>papers</Code>). They
                    are created on first use and do not nest.
                </li>
                <li>
                    <strong>Search</strong> is keyword search (BM25) and, when an embedding model is installed, semantic
                    search too, fused into one ranking. <Code>--rerank</Code> reorders the top results with a second
                    model, slower but better.
                </li>
                <li>
                    <strong>Folders of code</strong> are filed whole with <Code>kb add --dir</Code>, as git sees them.
                    Filing the same folder again only reads what changed.
                </li>
                <li>
                    <strong>kb fetches nothing.</strong> It has no HTTP client: the agent (or you) hands it the content.
                    The models are loaded from disk, and embeddings are computed in-process, on the CPU.
                </li>
            </ul>
            <Block>{`
kb init                                          # a store in this folder
kb add --title "io_uring_enter(2)" --collection io-uring \\
       --url https://man7.org/linux/man-pages/man2/io_uring_enter.2.html --file enter.md
kb add --dir ../liburing --collection io-uring   # a whole folder of source
kb search "submission queue polling" --collection io-uring
kb chunk C-1234                                  # one passage in full
kb stale --older-than 90d                        # what may be out of date
`}</Block>
            <Paragraph>
                Agents use kb through the <Code>kb</Code> MCP server, which has six tools: <Code>kb_search</Code>,{" "}
                <Code>kb_get</Code>, <Code>kb_add</Code>, <Code>kb_collections</Code>, <Code>kb_links</Code> and{" "}
                <Code>kb_stale</Code>. There is deliberately no tool to delete anything: forgetting is your decision.
            </Paragraph>
            <Paragraph>
                Commit <Code>.kb/</Code>'s logs and blobs if you want to keep the research with the project;{" "}
                <Code>index/</Code> is machine-local and ignored, and <Code>kb rebuild</Code> recreates it.
            </Paragraph>

            <Heading level={3}>Installing the models</Heading>
            <Paragraph>
                Keyword search works without any model. Semantic search needs an embedding model, and{" "}
                <Code>--rerank</Code> a reranker, in <Code>~/.kb/models/</Code>, one folder shared by every workspace.
                kb only reads it and never downloads anything, so you put the files there yourself, once.
            </Paragraph>
            <Paragraph>
                <strong>The quick way: one download.</strong>{" "}
                <Link href="https://procode.s3.fr-par.scw.cloud/kb-models.zip">kb-models.zip</Link> (632 MB) holds all
                three models and unzips straight into the folder:
            </Paragraph>
            <Block>{`
mkdir -p ~/.kb/models && cd ~/.kb/models
curl -fLO https://procode.s3.fr-par.scw.cloud/kb-models.zip   # or: wget https://procode.s3.fr-par.scw.cloud/kb-models.zip
unzip -o kb-models.zip && rm kb-models.zip
shasum -a 256 -c SHA256SUMS                                   # Linux: sha256sum -c SHA256SUMS
`}</Block>
            <Paragraph>On Windows, in PowerShell:</Paragraph>
            <Block>{`
New-Item -ItemType Directory -Force "$HOME\\.kb\\models" | Out-Null; Set-Location "$HOME\\.kb\\models"
Invoke-WebRequest https://procode.s3.fr-par.scw.cloud/kb-models.zip -OutFile kb-models.zip
Expand-Archive kb-models.zip -DestinationPath . -Force; Remove-Item kb-models.zip
`}</Block>
            <ul>
                <li>
                    <strong>gte-modernbert-base</strong>: the default embedder, one model for prose and code alike.
                </li>
                <li>
                    <strong>gte-reranker-modernbert-base</strong>: the reranker <Code>--rerank</Code> uses.
                </li>
                <li>
                    <strong>nomic-embed-text-v1.5</strong>: a smaller alternative embedder.
                </li>
            </ul>
            <Paragraph>
                All three are Apache-2.0. The zip's <Code>LICENSE</Code> and <Code>NOTICE.md</Code> say where each
                comes from and how it was converted, and <Code>SHA256SUMS</Code> lists their checksums.
            </Paragraph>
            <Paragraph>
                <strong>Building them yourself.</strong> The two gte files are converted from their Hugging Face
                weights with a script in the repository, which needs Python. The conversion is deterministic, so the
                same revision always gives the same file:
            </Paragraph>
            <Block>{`
cd cli/kb-cli/tools/modernbert
python -m venv venv
venv/bin/pip install torch transformers sentence-transformers gguf safetensors numpy
venv/bin/python -c "from huggingface_hub import snapshot_download as d; \\
  print(d('Alibaba-NLP/gte-modernbert-base', revision='e7f32e3c00f91d699e8c43b53106206bcc72bb22'))"
venv/bin/python convert.py <the folder it printed> ~/.kb/models/gte-modernbert-base.F16.gguf \\
  --revision e7f32e3c00f91d699e8c43b53106206bcc72bb22 --name gte-modernbert-base
`}</Block>
            <Paragraph>
                The reranker that <Code>--rerank</Code> uses, gte-reranker-modernbert-base, is made the same way at
                revision <Code>f7481e6055501a30fb19d090657df9ec1f79ab2c</Code>. The revisions and checksums are in{" "}
                <Link href={`${BLOB}/cli/kb-cli/tools/modernbert/MODERNBERT.md`}>MODERNBERT.md</Link>.
            </Paragraph>
            <ul>
                <li>
                    With several embedders in the folder, kb uses <Code>gte-modernbert-base.F16.gguf</Code>, and refuses
                    to guess without it.
                </li>
                <li>
                    A store remembers which model it was indexed with. Put a different model in place and searches
                    refuse with <Code>model_mismatch</Code> until <Code>kb reindex</Code> has embedded the store again.
                </li>
                <li>
                    Filing embeds for a limited time (20 seconds by default); what is left is searchable by keyword at
                    once, and <Code>kb embed</Code> (or <strong>Knowledge: Finish Embedding</strong>) finishes it.
                </li>
            </ul>
        </>
    ),
};

const coboard: Section = {
    id: "coboard",
    title: "Board",
    heading: "coboard, the board",
    body: (
        <>
            <Paragraph size="lg">
                coboard is a very simple board of epics, milestones and tickets. It is meant as a more robust
                alternative to an agent's plan: you can always see where the work is, comment on tickets, and read
                what the agent did for each one.
            </Paragraph>
            <Shot src={coboardShot} alt="The Board view: epics, milestones and tickets, with a ticket open" caption="The Board, with a ticket and its lap sessions open." />

            <Heading level={3}>How it works</Heading>
            <ul>
                <li>
                    Epics (<Code>E-1</Code>) hold milestones (<Code>M-1</Code>) and tickets (<Code>T-1</Code>). A ticket
                    is <em>todo</em>, <em>doing</em>, <em>blocked</em>, <em>review</em> or <em>done</em>, with a size
                    and a priority. Descriptions and comments are Markdown, and mention other items by id.
                </li>
                <li>
                    Agents work the board through the <Code>coboard</Code> MCP server; you work it in the extension's{" "}
                    <strong>Board</strong> view. Both write the same file, and the view refreshes as the agent writes.
                </li>
                <li>
                    The board is one append-only log, <Code>.coboard/log.jsonl</Code>. Commit it. Writes take a lock,
                    so agents in several folders can share it.
                </li>
                <li>
                    Finished work can be archived to keep lists short (right-click, <strong>Archive</strong> or{" "}
                    <strong>Archive with Note…</strong>), and brought back with <strong>Unarchive</strong>. Nothing is
                    ever deleted by an agent.
                </li>
                <li>
                    A project has one board, whatever folder you work in: in a lap branch folder, coboard uses the
                    parent folder's board. <strong>Board › Board Folder</strong> (or <Code>COBOARD_DIR</Code> for the
                    MCP server) points it at another folder's board.
                </li>
            </ul>

            <Heading level={3}>How it uses lap</Heading>
            <Paragraph>
                The board works fine without lap. With lap, a ticket also shows the work done for it, and the board
                stores nothing extra to do so. The link is a tag on the lap session:
            </Paragraph>
            <Block>{`
lap session start "T-12: fix the parser" --meta ticket=T-12
`}</Block>
            <Paragraph>
                coboard then asks lap (it runs the <Code>lap</Code> binary with <Code>--json</Code>) for every session
                tagged with the ticket, and for their commits. The ticket's tab lists those sessions, their commits
                grouped by intent, and how each session ended. Clicking a commit opens the same diff Lap History shows.
                Agents get the same through <Code>board_sessions</Code>. You can also start a session for a ticket from
                the Board with <strong>Start lap Session for Ticket</strong>.
            </Paragraph>
            <Paragraph>
                The <Link href={`${BLOB}/.claude/skills/tickets/SKILL.md`}>tickets skill</Link> is the whole loop used in
                procode's own repository (pick a ticket, move it to doing, record the work in a lap session, commit,
                close it). Adapt it for yours.
            </Paragraph>
        </>
    ),
};

const lapHistory: Section = {
    id: "lap-history",
    title: "Lap History",
    body: (
        <>
            <Paragraph size="lg">
                Lap History is the extension's view of a lap repository: a live tree of sessions and their commits,
                each with its intent and behavior.
            </Paragraph>
            <Shot src={lapShot} alt="The Lap History view beside a diff of one commit" caption="Lap History, with one commit's diff and its description." />
            <ul>
                <li>
                    Sessions are listed newest first, with their purpose, commit count and a marker on the active one.
                    A toggle switches to a flat list of every commit. The filter bar matches ids, hashes, intents,
                    behaviors and file paths.
                </li>
                <li>
                    Every commit shows its id (<Code>L23</Code>), short hash, file and line range. Open it for the
                    intent and behavior; references to other commits (<Code>#fa9cebd</Code>, <Code>L12</Code>) are
                    links.
                </li>
                <li>
                    Clicking a commit opens VS Code's own diff editor on the file just before and just after that edit,
                    scrolled to it, with the commit's description attached as an inline comment.
                </li>
                <li>The status bar shows the active session.</li>
                <li>It updates live: it watches <Code>.lap/log/</Code> and refreshes as the agent commits.</li>
            </ul>
            <Paragraph>
                It is <strong>view-only on purpose</strong>. Agents drive lap through the CLI; the view only shows what
                they did, as they do it. It reads the log directly, so it only runs <Code>lap</Code> to resolve a
                reference.
            </Paragraph>
        </>
    ),
};

const knowledge: Section = {
    id: "knowledge",
    title: "Knowledge",
    heading: "Knowledge, the kb view",
    body: (
        <>
            <Paragraph size="lg">
                Knowledge is where you browse and search the store that agents fill. Its job is small: find a
                document, read it, see where it came from.
            </Paragraph>
            <Shot src={kbShot} alt="The Knowledge view: collections, a document, and search" caption="Knowledge, with a collection and a document open." />

            <Heading level={3}>Indexing documents</Heading>
            <Paragraph>
                Run <strong>Knowledge: Initialise A Store In This Workspace</strong> once (or <Code>kb init</Code> in
                the project root). Then documents can be added from the view's title bar or the Command Palette:
            </Paragraph>
            <Grid
                head={["Command", "What it files"]}
                rows={[
                    [<strong>Knowledge: Add The Current File</strong>, "The file open in the editor."],
                    [<strong>Knowledge: Add Files…</strong>, "Files you pick."],
                    [
                        <strong>Knowledge: Add Folder…</strong>,
                        <>A whole folder, as git sees it (<Code>kb add --dir</Code>). Adding it again files only what changed.</>,
                    ],
                    [
                        <strong>Knowledge: Add A URL</strong>,
                        "A web page. The extension fetches it (kb itself fetches nothing) and files it with its URL and fetch date.",
                    ],
                    [<strong>Knowledge: Refresh Stale Documents</strong>, "Reads old documents from their source again, and re-indexes the ones whose text changed."],
                    [<strong>Knowledge: Finish Embedding</strong>, "Embeds the chunks a filing left for later, so semantic search sees them."],
                ]}
            />
            <Paragraph>
                You pick the collection each time. Most of the store, though, is filled by agents: the kb skill teaches
                them to search before fetching and to file what they read as they go.
            </Paragraph>

            <Heading level={3}>Reading</Heading>
            <ul>
                <li>
                    The sidebar opens on the collections; a collection opens its documents, a page at a time, with size,
                    type, fetch date and description. A collection filed from a folder is drawn as that folder's tree.
                </li>
                <li>The search bar searches the whole store, or the open collection.</li>
                <li>
                    A document shows where it came from, when it was fetched, and a <em>stale</em> badge when it is
                    old. A document filed from a local file opens that file.
                </li>
                <li>
                    <strong>Knowledge: Graph of Links</strong> draws how documents link to each other (
                    <em>supersedes</em>, <em>cites</em>, <em>implements</em>…).
                </li>
                <li>Everything is read-only: an indexed copy of someone else's documentation is never edited.</li>
            </ul>
        </>
    ),
};

const techdocs: Section = {
    id: "techdocs",
    title: "techdocs",
    body: (
        <>
            <Paragraph size="lg">
                techdocs pages are what an agent publishes for you to read in the editor: reports, comparisons,
                findings, anything with a table or a chart. They are close to Claude's artifacts, except that they
                stay in your project and look like part of VS Code.
            </Paragraph>
            <Shot src={techdocsShot} alt="A techdocs report open in a VS Code editor tab" caption="A techdocs page, in the editor's theme." />

            <Heading level={3}>How it works</Heading>
            <ul>
                <li>
                    The agent calls <Code>techdocs_publish</Code> on the <Code>techdocs</Code> MCP server with a title,
                    the page's HTML, a description and a few keywords. <Code>techdocs_template</Code> gives it a
                    starting page with every component in place.
                </li>
                <li>
                    Each page is stored as <Code>.techdocs/A-&lt;n&gt;/index.html</Code> with its{" "}
                    <Code>page.json</Code>. <Code>.techdocs/</Code> belongs in git: a result worth reading is worth
                    keeping with the work it came from.
                </li>
                <li>
                    The <strong>techdocs</strong> view lists every page, newest first, with the same filter bar as the
                    Board and Lap History: type to match titles and descriptions, or pick keywords.
                </li>
                <li>
                    A page opens in an editor tab. There is no delete tool for agents; removing a page is your call,
                    from the view.
                </li>
            </ul>

            <Heading level={3}>Why pages follow your theme</Heading>
            <Paragraph>
                Every UI in procode is built with <Link href="https://github.com/TypeFox/baukasten">baukasten</Link>, a UI
                toolkit whose design tokens (<Code>--bk-color-foreground</Code>, <Code>--bk-color-primary</Code>,{" "}
                <Code>--bk-spacing-4</Code>, …) are bound to VS Code's own theme variables. techdocs pages are written
                against those same tokens instead of fixed colours.
            </Paragraph>
            <Paragraph>When a page opens, the viewer puts in front of it, in order:</Paragraph>
            <ol>
                <li>baukasten's VS Code stylesheet, which defines the <Code>--bk-*</Code> tokens from <Code>--vscode-*</Code> variables;</li>
                <li>the current values of every <Code>--vscode-*</Code> variable;</li>
                <li>
                    a default stylesheet for plain elements (headings, tables, code…) and for a small set of components
                    by class name: callouts, KPI tiles, chips, tags, panels, charts.
                </li>
            </ol>
            <Paragraph>
                So a page with no CSS of its own already looks native, in light, dark or high-contrast themes, and when
                you switch themes the variables are swapped and the page follows. A page that hard-codes colours will
                look wrong in some theme; the tokens are the point.
            </Paragraph>
            <Paragraph>
                The page format is in <Link href={`${BLOB}/specs/techdocs.md`}>specs/techdocs.md</Link>.
            </Paragraph>
        </>
    ),
};

const commands: Section = {
    id: "commands",
    title: "Commands",
    heading: "Commands and tools",
    body: (
        <>
            <Paragraph size="lg">
                Everything you can run: the extension's commands, the two CLIs, and the MCP tools agents get.
            </Paragraph>

            <Heading level={3}>In VS Code</Heading>
            <Paragraph>From the Command Palette, or the views' title bars and right-click menus.</Paragraph>
            <Grid
                head={["Command", "What it does"]}
                rows={[
                    [<strong>procode: Set Up MCP for Claude Code</strong>, <>Adds <Code>coboard</Code>, <Code>kb</Code> and <Code>techdocs</Code> to the project's <Code>.mcp.json</Code>.</>],
                    [<strong>procode: Add Skills for Claude Code</strong>, <>Adds the skills you pick to the project's <Code>.claude/skills/</Code>.</>],
                    [<strong>Board: New Epic / New Milestone / New Ticket</strong>, "Adds an item to the board."],
                    [<strong>Board: Archive / Archive with Note… / Unarchive</strong>, "Takes finished work out of the lists, or brings it back."],
                    [<strong>Board: Close / Reopen</strong>, "Closes an epic or a milestone, or opens it again."],
                    [<strong>Board: Start lap Session for Ticket</strong>, <>Starts a lap session tagged with the ticket.</>],
                    [<strong>Lap: Toggle Session Grouping / Raw List</strong>, "Sessions, or every commit in one list."],
                    [<strong>Lap: Show Commit</strong>, "Opens a commit's diff."],
                    [<strong>Knowledge: Initialise A Store In This Workspace</strong>, <>Runs <Code>kb init</Code>.</>],
                    [<strong>Knowledge: Add…</strong>, "The current file, files, a folder or a URL (see Knowledge)."],
                    [<strong>Knowledge: Search / Collections / Graph of Links</strong>, "Finds and browses what is filed."],
                    [<strong>Knowledge: Refresh Stale Documents / Finish Embedding</strong>, "Keeps the store current."],
                    [<strong>techdocs: Open Page… / Open HTML Source / Delete</strong>, "Reads, inspects or removes a page."],
                ]}
            />

            <Heading level={3}>lap</Heading>
            <Grid
                head={["Command", "What it does"]}
                rows={[
                    [<Code>lap init</Code>, <>Creates <Code>.lap/</Code> and a starter <Code>.lapignore</Code>.</>],
                    [<Code>lap status</Code>, "Pending edits per file, numbered."],
                    [<Code>lap commit &lt;file&gt; -i … -b …</Code>, <>Records one edit (<Code>--edit N</Code>, <Code>--lines A-B</Code> to pick one).</>],
                    [<Code>lap amend &lt;commit&gt; -i … -b …</Code>, "Corrects what a commit says, without rewriting anything."],
                    [<Code>lap session start | end | list | current</Code>, "Groups commits into tasks."],
                    [<Code>lap log</Code>, <>Commits, newest first (<Code>--session</Code>, <Code>--file</Code>).</>],
                    [<Code>lap show &lt;commit&gt;</Code>, <>One commit in full (<Code>--full-file</Code> rebuilds the file as of it).</>],
                    [<Code>lap search</Code>, <>Blame a line (<Code>--file F --line N</Code>), find text (<Code>--text</Code>) or messages (<Code>--msg</Code>).</>],
                    [<Code>lap rr &lt;session&gt;</Code>, "A review request: the whole task, grouped by intent, with the net change."],
                    [<><Code>lap verify</Code> / <Code>lap rebuild</Code></>, "Checks the hash chain; rebuilds the caches."],
                    [<Code>lap branch start | list | forget | move</Code>, "Makes a folder a branch of another, and tends the list."],
                    [<Code>lap merge &lt;branch&gt;</Code>, <>After <Code>git merge</Code>, adopts the branch's history (<Code>--dry-run</Code> first).</>],
                ]}
            />

            <Heading level={3}>kb</Heading>
            <Grid
                head={["Command", "What it does"]}
                rows={[
                    [<Code>kb init</Code>, <>Creates a store, <Code>.kb/</Code>, in the current folder.</>],
                    [<Code>kb add</Code>, <>Files a document (<Code>--file</Code> or stdin), many (<Code>--batch</Code>) or a folder (<Code>--dir</Code>).</>],
                    [<Code>kb search &lt;query&gt;</Code>, <>Snippets, ranked (<Code>--collection</Code>, <Code>--mode keyword</Code>, <Code>--rerank</Code>).</>],
                    [<><Code>kb chunk &lt;C-n&gt;</Code> / <Code>kb get &lt;D-n&gt;</Code></>, "One passage in full; one document."],
                    [<><Code>kb ls</Code> / <Code>kb collections</Code> / <Code>kb sources</Code></>, "What is filed."],
                    [<><Code>kb stale</Code> / <Code>kb refresh</Code></>, "Old documents; read file sources again."],
                    [<Code>kb links</Code>, "Links between documents: show, add, delete."],
                    [<Code>kb forget &lt;D-n|S-n&gt;</Code>, "Forgets a document or a source."],
                    [<><Code>kb embed</Code> / <Code>kb reindex</Code> / <Code>kb rebuild</Code> / <Code>kb compact</Code></>, "Maintenance."],
                ]}
            />
            <Paragraph>
                Every command of both CLIs answers <Code>--help</Code> and takes <Code>--json</Code>.
            </Paragraph>

            <Heading level={3}>MCP tools</Heading>
            <Grid
                head={["Server", "Tools"]}
                rows={[
                    [
                        <Code>coboard</Code>,
                        <>
                            <Code>board_list</Code>, <Code>board_get</Code>, <Code>board_search</Code>, <Code>board_create</Code>,{" "}
                            <Code>board_update</Code>, <Code>board_move</Code>, <Code>board_comment</Code>,{" "}
                            <Code>board_sessions</Code>, <Code>board_archive</Code>, <Code>board_unarchive</Code>
                        </>,
                    ],
                    [
                        <Code>kb</Code>,
                        <>
                            <Code>kb_search</Code>, <Code>kb_get</Code>, <Code>kb_add</Code>, <Code>kb_collections</Code>,{" "}
                            <Code>kb_links</Code>, <Code>kb_stale</Code>
                        </>,
                    ],
                    [
                        <Code>techdocs</Code>,
                        <>
                            <Code>techdocs_publish</Code>, <Code>techdocs_template</Code>, <Code>techdocs_list</Code>,{" "}
                            <Code>techdocs_get</Code>
                        </>,
                    ],
                ]}
            />
            <Paragraph>
                None of the servers can delete: not a ticket, not a document, not a page. Those are left to you. lap has
                no MCP server; agents run the CLI, as the lap skill teaches them.
            </Paragraph>
            <Paragraph>Without VS Code, point any MCP client at the servers, started inside the project:</Paragraph>
            <Block>{`
{
  "mcpServers": {
    "coboard":  { "command": "/path/to/procode/packages/coboard/bin/coboard-mcp" },
    "kb":       { "command": "/path/to/procode/packages/kb-mcp/bin/kb-mcp", "env": { "KB_BIN": "kb" } },
    "techdocs": { "command": "/path/to/procode/packages/techdocs/bin/techdocs-mcp" }
  }
}
`}</Block>
        </>
    ),
};

const security: Section = {
    id: "security",
    title: "Security",
    body: (
        <>
            <Callout kind="danger" title="A techdocs page is HTML, rendered">
                <Paragraph>
                    When you open a techdocs page, VS Code renders the HTML the agent wrote, and its scripts run. Treat a
                    page the way you would treat an HTML file someone sent you.
                </Paragraph>
            </Callout>
            <Paragraph>What the viewer does to contain a page:</Paragraph>
            <ul>
                <li>
                    It runs in a sandboxed frame (<Code>sandbox="allow-scripts"</Code>, no same-origin). The page's
                    scripts cannot reach VS Code, the extension, your workspace or any storage.
                </li>
                <li>
                    A content security policy of <Code>default-src 'none'</Code> blocks the network: no fetch, and no
                    remote script, image, font or stylesheet. Only inline code and <Code>data:</Code> URIs load.
                </li>
            </ul>
            <Paragraph>What that does not protect you from:</Paragraph>
            <ul>
                <li>
                    <strong>What the page says.</strong> A page can show anything: a wrong conclusion, a made-up number, a
                    convincing "run this command" box. It is the agent's work and deserves the same review as its code.
                </li>
                <li>
                    <strong>Pages you did not ask for.</strong> <Code>.techdocs/</Code> is committed to git, so a repository
                    you clone or a pull request you check out can bring pages written by someone else, or by an agent
                    someone else steered. Use <strong>Open HTML Source</strong> to read one before opening it.
                </li>
                <li>
                    <strong>Scripts that misbehave.</strong> Inline scripts run. The sandbox keeps them away from your
                    files, not from your CPU.
                </li>
            </ul>

            <Heading level={3}>Other things that come in</Heading>
            <ul>
                <li>
                    <strong>What kb files.</strong> Web pages and documentation filed into kb are text written by
                    strangers, and an agent will read them back later as if they were reference material. A page can
                    hide instructions aimed at an agent (a prompt injection). Be careful what you file, and what you let
                    an agent file unattended. The Knowledge view never renders a filed page as HTML.
                </li>
                <li>
                    <strong>Other people's history and board.</strong> <Code>.lap/log/</Code>,{" "}
                    <Code>.coboard/log.jsonl</Code> and <Code>.kb/</Code> are plain files in the repository. In a
                    project you did not start, their intents, tickets and comments are text anyone could have written,
                    and your agent will read them.
                </li>
                <li>
                    <strong>Skills and <Code>.mcp.json</Code>.</strong> A skill is instructions your agent follows, and
                    an MCP server is a program it runs. Only use ones you have read.
                </li>
            </ul>
        </>
    ),
};

const faq: Section = {
    id: "faq",
    title: "FAQ",
    body: (
        <Accordion className="faq">
            <Faq q="Does it make my agent use more tokens?">
                <Paragraph>
                    Yes. In my experiments, about 10 to 15% more on average. lap is a CLI, so its share is hard to measure.
                    What you get for it is a record of why every line changed, a board you can follow, and research that
                    is not fetched twice.
                </Paragraph>
            </Faq>
            <Faq q="Which agents does it work with?">
                <Paragraph>
                    Any agent that can run a command and talk to an MCP server, and humans too. Only Claude Code is tested.
                    VS Code's own agent picks up the MCP servers without any setup; Claude Code needs the two setup
                    commands in <Link href="#intro">Getting started</Link>.
                </Paragraph>
            </Faq>
            <Faq q="Can I use only one part?">
                <Paragraph>
                    Yes. Hide the views you do not use (right-click a view's header). The board works without lap,
                    techdocs needs neither CLI, and lap is a standalone binary. Each view is also its own extension in the
                    repository if you build from source, though only procode itself sets up the MCP servers and skills.
                </Paragraph>
            </Faq>
            <Faq q="Does lap replace git?">
                <Paragraph>
                    No. lap sits below git and never touches it: you still commit with git, and git carries lap's history (
                    <Code>.lap/log/</Code>) like any other file. lap cannot restore anything either; to see an old version,
                    use <Code>lap show --full-file</Code>.
                </Paragraph>
                <Paragraph>
                    One thing to know: lap does not watch git. Files that <Code>git checkout</Code>, <Code>pull</Code> or{" "}
                    <Code>rebase</Code> change look like your edits to lap, so commit or discard before switching branches.
                </Paragraph>
            </Faq>
            <Faq q="Why is it not for teams?">
                <Paragraph>
                    lap's history and the board are append-only logs committed to git. If two people, or two git branches
                    of one folder, both add to them, merging conflicts: each side has handed out the same next ids, and
                    interleaving lap's lines breaks its hash chain. No resolution keeps both sides.
                </Paragraph>
            </Faq>
            <Faq q="Can agents work in parallel?">
                <Paragraph>
                    Yes, each in its own folder (usually a git worktree) made a lap branch with{" "}
                    <Code>lap branch start &lt;name&gt; --from &lt;first folder&gt;</Code>. A branch records to files of
                    its own and uses the first folder's board. To bring it back, in the first folder:{" "}
                    <Code>git merge</Code>, then <Code>lap merge &lt;name&gt;</Code>. Never merge the parent into a branch
                    to stay current: merge the branch back and start a new one.
                </Paragraph>
            </Faq>
            <Faq q="Where is everything stored, and what should I commit?">
                <Grid
                    head={["Where", "What", "Commit it?"]}
                    rows={[
                        [<Code>.lap/log/</Code>, "lap's history", "Yes (the rest of .lap/ is ignored for you)"],
                        [<Code>.coboard/log.jsonl</Code>, "the board", "Yes"],
                        [<Code>.techdocs/</Code>, "techdocs pages", "Yes"],
                        [<Code>.kb/</Code>, "kb's logs, blobs and index", <>Logs and blobs if you want to keep the research; never <Code>index/</Code></>],
                        [<Code>~/.procode/bin</Code>, <>the <Code>lap</Code> and <Code>kb</Code> procode runs</>, "Outside the project"],
                        [<Code>~/.kb/models</Code>, "embedding models", "Outside the project"],
                    ]}
                />
            </Faq>
            <Faq q="Does anything leave my machine?">
                <Paragraph>
                    No. lap, kb, the board and techdocs work on local files. kb never downloads anything, not even its
                    models. The one exception is <strong>Knowledge: Add A URL</strong>, which fetches the page you give it.
                    (Your agent, of course, talks to its own provider.)
                </Paragraph>
            </Faq>
            <Faq q="Do I need an embedding model for kb?">
                <Paragraph>
                    No. Keyword search works without one. A model adds semantic search: finding passages that mean the
                    same thing in other words. All of kb's models come in one download,{" "}
                    <Link href="https://procode.s3.fr-par.scw.cloud/kb-models.zip">kb-models.zip</Link>, unzipped into{" "}
                    <Code>~/.kb/models/</Code>; see <Link href="#kb">kb</Link>.
                </Paragraph>
            </Faq>
            <Faq q="Which platforms are supported?">
                <Paragraph>
                    The extension packages with CLIs are for macOS (Apple silicon and Intel), Linux and Alpine (x64 and
                    arm64), and Windows (x64 and arm64). Development happens on macOS. On any other platform, install the
                    package without CLIs and build them with CMake and a C11 compiler.
                </Paragraph>
            </Faq>
            <Faq q="How do I uninstall it?">
                <Paragraph>
                    Uninstall the extension, then delete <Code>~/.procode</Code> (and <Code>~/.kb</Code> for the models)
                    yourself: procode never deletes anything there. In a project, remove the servers from{" "}
                    <Code>.mcp.json</Code>, the skills from <Code>.claude/skills/</Code>, and the stores (
                    <Code>.lap/</Code>, <Code>.kb/</Code>, <Code>.coboard/</Code>, <Code>.techdocs/</Code>) if you no
                    longer want them.
                </Paragraph>
            </Faq>
            <Faq q="Where do I report a bug or ask a question?">
                <Paragraph>
                    On <Link href={`${REPO}/issues`}>GitHub issues</Link>.
                </Paragraph>
            </Faq>
        </Accordion>
    ),
};

export const SECTIONS: readonly Section[] = [intro, lap, kb, coboard, lapHistory, knowledge, techdocs, commands, security, faq];
