/* The checks that are about the whole package rather than about one function.
 *
 * A unit test pins the behaviour of the code it calls. These pin the shape of
 * the code nobody called: that no `FileSystemProvider` is registered, in any
 * file, including the compiled output that actually runs; that no two-value
 * padding token is composed into a shorthand; that every `--bk-*` name this
 * package uses is one the shipped token layer defines. Each of them is a
 * mutation that would pass every other test in this directory.
 */

import * as assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";

import { KNOWLEDGE_STYLESHEETS, cssAssets, knowledgePolicy, policyPermits, sourcesOf } from "../policy";
import { OPERATIONS } from "../protocol";

const ROOT = path.resolve(__dirname, "..", "..");

/* The files whose job is to look for the things below, and which therefore
 * have to be able to spell them. Named rather than pattern-matched, because a
 * pattern is how a file quietly joins the list — the whole value of these
 * checks is that the set of places a forbidden token may appear is small
 * enough to read. */
function isChecker(file: string): boolean {
    return (
        file.startsWith(path.join("src", "test")) ||
        file === path.join("scripts", "packaged-smoke.mjs")
    );
}

/** Every source file this package owns, as text. */
function sources(): { file: string; text: string }[] {
    const out: { file: string; text: string }[] = [];
    const walk = (dir: string): void => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const p = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(p);
            } else if (/\.(ts|tsx|mjs|css|json)$/.test(entry.name)) {
                out.push({ file: path.relative(ROOT, p), text: fs.readFileSync(p, "utf8") });
            }
        }
    };
    for (const dir of ["src", "webview", "assets", "scripts"]) {
        walk(path.join(ROOT, dir));
    }
    out.push({
        file: "package.json",
        text: fs.readFileSync(path.join(ROOT, "package.json"), "utf8"),
    });
    return out;
}

/** Comments stripped, because a file's prose has to be able to name what it refuses. */
function code(text: string): string {
    return text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
}

/* baukasten's built files. The npm workspace hoists dependencies to the
 * repository root, so the package's own node_modules is only the first place
 * to look. */
function baukastenDist(): string {
    const local = path.join(ROOT, "node_modules", "baukasten-ui", "dist");
    return fs.existsSync(local) ? local : path.join(ROOT, "..", "..", "node_modules", "baukasten-ui", "dist");
}

function bundle(): string {
    const file = path.join(ROOT, "out", "media", "knowledge-webview.js");
    assert.ok(fs.existsSync(file), "the webview bundle has not been built");
    return fs.readFileSync(file, "utf8");
}

function stylesheet(): string {
    return fs.readFileSync(path.join(ROOT, "assets", "knowledge.css"), "utf8");
}

/* ------------------------------------------- the provider that is not there */

test("the kb scheme has no filesystem provider, because that is what draws the header", () => {
    /* index-ui.md §6. `kb:/D-241` is an address, not a file, and VSCode decides
     * which of those it believes by one question: whether the file service has
     * a provider for the scheme. `breadcrumbsControl.ts` hides on exactly
     *
     *     if (!uri || !this._fileService.hasProvider(uri)) { ...this.hide(); }
     *
     * and the editor-type dropdown — "switch between the editors that can open
     * the active resource" — is drawn by the same control behind the same
     * guard. So registering a provider, even one that answers every read with
     * an empty file, puts a header on every document tab naming a path that
     * does not exist and offering an editor that cannot open it.
     *
     * A source scan rather than a test of behaviour, because there is no
     * behaviour to test: the failure is a call that must never appear. */
    for (const { file, text } of sources()) {
        if (isChecker(file) || !file.startsWith("src")) {
            continue;
        }
        assert.ok(
            !code(text).includes("registerFileSystemProvider"),
            `${file} registers a filesystem provider; a kb: URI is an address and a provider would put a breadcrumb and an editor picker above every document tab`,
        );
    }
    /* And in what ships, because a source-only assertion would miss a provider
     * that arrived through a dependency or a bundler alias. */
    const out = path.join(ROOT, "out", "extension.js");
    assert.ok(fs.existsSync(out), "the extension has not been built");
    assert.ok(
        !fs.readFileSync(out, "utf8").includes("registerFileSystemProvider"),
        "the compiled extension registers a filesystem provider",
    );
});

test("the document editor is read-only, and says so in the type it implements", () => {
    /* §3: "editing an indexed copy of someone else's documentation would make
     * the content hash meaningless and the provenance a lie."
     * `CustomReadonlyEditorProvider` is what keeps VSCode from offering a save
     * that means nothing and from prompting on close — and the editable
     * interface is one word away, compiles identically at the call site, and
     * would show a dirty dot on a tab nobody can save. */
    const editor = fs.readFileSync(path.join(ROOT, "src", "editor.ts"), "utf8");
    assert.match(
        editor,
        /implements\s+vscode\.CustomReadonlyEditorProvider/,
        "the editor no longer implements the read-only provider interface",
    );
    for (const { file, text } of sources()) {
        if (isChecker(file)) {
            continue;
        }
        /* `vscode.CustomEditorProvider` is the EDITABLE interface and is one
         * word away from the read-only one. `registerCustomEditorProvider` —
         * the function both are passed to — necessarily contains the same
         * letters, so the check is on the qualified type name. */
        assert.ok(
            !/vscode\.CustomEditorProvider\b/.test(code(text)),
            `${file} names the editable custom editor interface`,
        );
        assert.ok(
            !/\bonDidChangeCustomDocument\b/.test(code(text)),
            `${file} declares an edit event, which is what makes a custom editor editable`,
        );
        /* And nothing in the rendered document offers to change it. */
        if (file.startsWith("webview")) {
            assert.ok(
                !/<textarea/.test(text) || file.endsWith("Collections.tsx"),
                `${file} renders a textarea; §3 is read-only`,
            );
        }
    }
    /* The rename field in the collections tab is the ONE input in this package
     * that writes: it edits a collection's NAME, which §4 puts there on
     * purpose. It is an `<input>` and not a `<textarea>`. The rail's search
     * field is the other `<input>`, and it writes nothing: it is the query, a
     * single element in the sidebar with the bar's class, and no other file
     * holds one. */
    const inputs = sources().filter(
        (s) => s.file.startsWith("webview") && /<input\b/.test(s.text),
    );
    assert.deepEqual(
        inputs.map((s) => s.file).sort(),
        [path.join("webview", "Collections.tsx"), path.join("webview", "Sidebar.tsx")].sort(),
        "an editable field appeared outside the collections tab, where §4 puts the only two writes",
    );
    const rail = inputs.find((s) => s.file.endsWith("Sidebar.tsx"))!.text;
    assert.equal((rail.match(/<input\b/g) ?? []).length, 1, "the rail holds more than its one search field");
    assert.match(rail, /<input\s+className="kb-search-input"/, "the rail's input is not the search field");
});

test("the compiled editor keeps one tab per document", () => {
    /* §6: "opening a document that already has a tab focuses it." VSCode does
     * that when the resource and the view type match — and only if the provider
     * is registered with `supportsMultipleEditorsPerDocument: false`. */
    const out = fs.readFileSync(path.join(ROOT, "out", "editor.js"), "utf8");
    assert.match(out, /supportsMultipleEditorsPerDocument:\s*false/);
});

/* ----------------------------------------------- nothing becomes markup */

test("nothing in this package sets innerHTML from data", () => {
    /* The store holds pages somebody else wrote. §3.2 renders them as prose,
     * and the whole of how that stays safe is that there is no HTML string:
     * `view/html.ts` emits nodes and React turns text into text. */
    /* COMMENTS STRIPPED, because the files that got this right are the files
     * whose prose has to spell the shape they are not using — `view/html.ts`
     * and `view/code.ts` both explain at length why they answer nodes and
     * tokens rather than markup, and neither can do that without naming it. */
    for (const { file, text } of sources()) {
        if (isChecker(file)) {
            continue;
        }
        const c = code(text);
        assert.ok(!c.includes("dangerouslySetInnerHTML"), `${file} sets innerHTML`);
        assert.ok(!/\.innerHTML\s*=/.test(c), `${file} assigns innerHTML`);
        assert.ok(!/\bsrcdoc\b/i.test(c), `${file} writes a document into a frame`);
        assert.ok(!/<iframe/i.test(c), `${file} frames something`);
    }
    /* NOT CHECKED IN THE BUNDLE, AND THE REASON IS WORTH WRITING DOWN RATHER
     * THAN LEAVING AS A GAP SOMEBODY REDISCOVERS. react-dom's own machinery
     * spells `dangerouslySetInnerHTML` thirteen times and `__html` five — it
     * has to, because reading that prop is how the escape hatch works — so a
     * name check over the bundle would fail on somebody else's code and teach
     * whoever hit it to delete the guard. What IS this package's to assert in
     * the bundle is that no markup for a match reached it, which the clamp
     * test below does. */
});

test("nothing renders an anchor whose href comes from a document", () => {
    /* A markdown or HTML link is rendered as a button that hands its href to
     * the host, never as an `<a href>`. An anchor would put a URL somebody else
     * wrote into the DOM of this webview — which is not sandboxed — and
     * `javascript:` in an href is a click away from running there. */
    for (const { file, text } of sources()) {
        if (!file.startsWith("webview") || isChecker(file)) {
            continue;
        }
        assert.ok(!/href=\{/.test(text), `${file} renders an anchor from data`);
        assert.ok(!/<a\s/.test(code(text)), `${file} renders an anchor`);
    }
});

test("the markdown renderer keeps kb's own scheme, defers on every other, and adds no raw HTML", () => {
    /* THE FIRST HALF IS A REMINDER AND THE PROOF CANNOT RUN HERE.
     * react-markdown's `defaultUrlTransform` keeps http, https, mailto and irc
     * and returns "" for everything else — so `kb:/D-241` in a document would
     * arrive at the renderer as an empty href and render as a control that
     * looks clickable and does nothing, with nothing thrown and nothing
     * logged. A real test would render the component, and it cannot:
     * `Markdown.tsx`'s neighbours call `acquireVsCodeApi()` at module load and
     * react-markdown is ESM while this test runs from a CommonJS build. */
    const file = path.join(ROOT, "webview", "Markdown.tsx");
    const text = fs.readFileSync(file, "utf8");
    assert.match(
        text,
        /urlTransform=\{/,
        "Markdown.tsx does not pass urlTransform, so react-markdown blanks every kb: href",
    );
    assert.ok(
        text.includes("defaultUrlTransform"),
        "Markdown.tsx's urlTransform does not fall back to react-markdown's, so it permits every scheme",
    );
    assert.ok(
        text.includes("KB_SCHEME"),
        "Markdown.tsx's urlTransform no longer names kb's scheme, so references are blanked again",
    );
    /* And the plugin that must NOT be there. Without `rehype-raw`,
     * react-markdown escapes raw HTML — which is the whole mechanism by which
     * a markdown document out of somebody else's site cannot smuggle an
     * element into this webview. */
    for (const { file: f, text: t } of sources()) {
        if (isChecker(f)) {
            continue;
        }
        assert.ok(
            !code(t).includes("rehype-raw"),
            `${f} adds rehype-raw, which re-enables raw HTML`,
        );
    }
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as {
        dependencies: Record<string, string>;
    };
    assert.ok(!("rehype-raw" in pkg.dependencies));
    for (const needed of ["react-markdown", "remark-gfm"]) {
        assert.ok(needed in pkg.dependencies, `${needed} is not a dependency`);
    }
});

test("this package adds no third-party dependency coboard-vscode does not already carry", () => {
    /* The rule for this slice: the view layer may cost react, react-dom,
     * baukasten and the markdown renderer, because a neighbouring package has
     * already paid for them and this one renders the same way. Anything else
     * is a new thing to audit, and it arrives one convenience at a time. */
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as {
        dependencies: Record<string, string>;
    };
    const neighbour = path.join(ROOT, "..", "coboard-vscode", "package.json");
    if (!fs.existsSync(neighbour)) {
        return;
    }
    const allowed = new Set([
        ...Object.keys(
            (JSON.parse(fs.readFileSync(neighbour, "utf8")) as { dependencies: Record<string, string> })
                .dependencies,
        ),
        /* This package's own sibling, which is the `lap-js`/`coboard-core`
         * slot one package along. */
        "kb-js",
        /* PDF text extraction (index-api §12.3), decided on its own ticket:
         * pure JavaScript so the package stays universal, Mozilla's, and
         * shipped beside the bundle (src/pdf.ts), not inside it. */
        "pdfjs-dist",
    ]);
    for (const dep of Object.keys(pkg.dependencies)) {
        assert.ok(
            allowed.has(dep),
            `${dep} is a dependency coboard-vscode does not have, so this package is paying a cost nothing else in the workspace has justified`,
        );
    }
});

/* ------------------------------------------------------- what ships */

test("the webview bundle carries no process, no extension host API, and a handle to one", () => {
    const text = bundle();
    /* `kb-js`'s main entry spawns a process, which a browser document cannot
     * do; `kb-js/pure` is what the view layer imports. The bundler refuses
     * `node:*` by name and this is the proof the refusal is switched on. */
    assert.ok(!text.includes("node:child_process"), "the spawn was bundled into the UI");
    assert.ok(!text.includes("node:fs"));
    assert.ok(!text.includes('require("vscode")'));
    assert.ok(text.includes("acquireVsCodeApi"), "the webview has no handle to the host");
});

test("nothing that reaches the webview imports the half of kb-js that spawns", () => {
    /* The bundler catches this at `node:child_process`, which is two files
     * further in and names the wrong file in its message. Said here in terms
     * of the import that caused it. */
    const reachable = ["webview", path.join("src", "view"), path.join("src", "uri.ts"), path.join("src", "protocol.ts")];
    for (const { file, text } of sources()) {
        if (isChecker(file) || !reachable.some((r) => file.startsWith(r))) {
            continue;
        }
        assert.ok(
            !/from\s+["']kb-js["']/.test(text),
            `${file} is compiled into the webview bundle and imports kb-js's main entry, which reaches node:child_process; kb-js/pure is the half a renderer needs`,
        );
    }
});

test("every asset the webview links is in out/media", () => {
    const media = path.join(ROOT, "out", "media");
    for (const name of [
        "baukasten-base.css",
        "baukasten-vscode.css",
        "knowledge.css",
        "codicon.css",
        "codicon.ttf",
        "knowledge-webview.js",
    ]) {
        assert.ok(fs.existsSync(path.join(media, name)), `out/media/${name} is missing`);
    }
    /* The web token layer pins fixed fallbacks and would ignore the user's
     * theme, so it must not be what gets shipped. */
    assert.ok(!fs.existsSync(path.join(media, "baukasten-web.css")));
    for (const { file, text } of sources()) {
        if (isChecker(file)) {
            continue;
        }
        assert.ok(
            !text.includes("baukasten-web.css"),
            `${file} names baukasten-web.css, which ignores the user's theme`,
        );
    }
});

/* --------------------------------------------------- the content policy */

/* VSCode's `webview.cspSource` as it actually arrives, which is two sources and
 * not one. Written out rather than mocked, because the thing being checked is a
 * string substitution into a policy and an approximation of the input would be
 * checking a different policy. */
const CSP_SOURCE = "'self' https://*.vscode-cdn.net";

test("every asset in a stylesheet the webview loads is permitted by the policy", () => {
    /* THE BUG THIS EXISTS FOR IS A LINE IN A CONSOLE NOBODY HAS OPEN.
     * `baukasten-base.css` inlines the codicon font as a `data:font/ttf` URI —
     * it is the only shipped stylesheet with a `url()` of any kind — and a
     * `font-src` carrying only `webview.cspSource` does not permit `data:`.
     * The font is blocked, every baukasten component drawing a codicon draws
     * nothing, and the surface looks unfinished rather than broken. */
    assert.deepEqual(KNOWLEDGE_STYLESHEETS, [
        "baukasten-base.css",
        "baukasten-vscode.css",
        "codicon.css",
        "knowledge.css",
    ]);
    const policy = knowledgePolicy(CSP_SOURCE, "abc123");
    const refused: string[] = [];
    for (const name of KNOWLEDGE_STYLESHEETS) {
        const file = path.join(ROOT, "out", "media", name);
        assert.ok(fs.existsSync(file), `out/media/${name} is missing; the document links it`);
        for (const asset of cssAssets(fs.readFileSync(file, "utf8"))) {
            if (!policyPermits(policy, asset, CSP_SOURCE)) {
                refused.push(`${name}: ${asset.directive} refuses ${asset.url.slice(0, 48)}`);
            }
        }
    }
    assert.deepEqual(refused, []);

    /* And the scan is doing something: the inlined font and the linked one are
     * both there to be found, so a parser that returned nothing would pass the
     * assertion above vacuously. */
    const base = cssAssets(fs.readFileSync(path.join(ROOT, "out", "media", "baukasten-base.css"), "utf8"));
    assert.ok(
        base.some((a) => a.directive === "font-src" && a.url.startsWith("data:font")),
        "baukasten-base.css no longer inlines a font; re-read this test before trusting it",
    );
    assert.deepEqual(
        cssAssets(fs.readFileSync(path.join(ROOT, "out", "media", "codicon.css"), "utf8")),
        [{ url: "./codicon.ttf", directive: "font-src" }],
    );
});

test("the policy is a nonce and nothing else where it matters", () => {
    const policy = knowledgePolicy(CSP_SOURCE, "abc123");
    assert.deepEqual(sourcesOf(policy, "script-src"), ["'nonce-abc123'"]);
    assert.ok(!sourcesOf(policy, "script-src").includes("'unsafe-inline'"));
    assert.ok(!sourcesOf(policy, "script-src").includes("'unsafe-eval'"));
    assert.ok(sourcesOf(policy, "font-src").includes("data:"));
    assert.deepEqual(sourcesOf(policy, "default-src"), ["'none'"]);
    /* An absent directive falls back to `default-src`, which is how a browser
     * reads it — so `connect-src` and `frame-src` are `'none'` without being
     * written, and nothing here reaches a network or navigates a frame. */
    assert.deepEqual(sourcesOf(policy, "connect-src"), ["'none'"]);
    assert.deepEqual(sourcesOf(policy, "frame-src"), ["'none'"]);
    assert.equal(
        policyPermits(policy, { url: "https://cdn.example.test/f.woff", directive: "font-src" }, CSP_SOURCE),
        false,
    );
});

test("host.ts writes the nonce it generates onto both of its scripts", () => {
    const host = code(fs.readFileSync(path.join(ROOT, "src", "host.ts"), "utf8"));
    assert.ok(host.includes("knowledgePolicy(webview.cspSource, n)"), "the policy is built from somewhere else");
    assert.equal(
        (host.match(/<script nonce="\$\{n\}"/g) ?? []).length,
        2,
        "a script in the webview document carries no nonce, so the policy blocks it",
    );
});

/* ------------------------------------------------------------- the theme */

test("every --bk-* token this package uses is one the shipped token layer defines", () => {
    /* THE BUG THIS EXISTS FOR IS SILENT. An undefined custom property makes its
     * whole declaration invalid at computed-value time, so a misspelled colour
     * token does not fall back to anything — the rule is dropped and the
     * element renders as though nobody had styled it.
     *
     * Read out of the installed package rather than from a list here, so a
     * baukasten upgrade that renames a token fails this test instead of
     * quietly unstyling a surface. */
    const dist = baukastenDist();
    const layer = ["baukasten-vscode.css", "baukasten-base.css"]
        .map((f) => path.join(dist, f))
        .filter((f) => fs.existsSync(f))
        .map((f) => fs.readFileSync(f, "utf8"))
        .join("\n");
    assert.ok(layer.length > 0, "the baukasten stylesheets are not installed; run npm install");
    /* A definition is `--bk-x: value`, a use is `var(--bk-x...)`. Matching the
     * colon is what keeps a use in the token layer from certifying itself. */
    const defined = new Set([...layer.matchAll(/(--bk-[a-z0-9-]+)\s*:/g)].map((m) => m[1]));
    assert.ok(defined.size > 50, `only ${defined.size} tokens were parsed; the scan is broken`);

    const bad: string[] = [];
    for (const { file, text } of sources()) {
        if (isChecker(file)) {
            continue;
        }
        for (const m of text.matchAll(/var\(\s*(--bk-[a-z0-9-]+)/g)) {
            if (!defined.has(m[1])) {
                bad.push(`${file}: ${m[1]}`);
            }
        }
    }
    assert.deepEqual(
        [...new Set(bad)].sort(),
        [],
        "these --bk-* names are not defined by baukasten, so every rule using one is dropped",
    );

    /* AND THE SCAN IS FINDING USES AT ALL. Everything above passes vacuously on
     * a stylesheet that has stopped reaching for the token layer — a literal
     * `#8c8c8c` in place of a `var()` is not a name this can fail on, it is a
     * name this never sees. The four below are what the surface's hierarchy is
     * made of: the weight that separates a title from what is under it, the
     * mute that lets the metadata recede, the face that holds an identifier
     * apart from the title beside it, and the colour §5's badge is drawn in. */
    const used = new Set([...stylesheet().matchAll(/var\(\s*(--bk-[a-z0-9-]+)/g)].map((m) => m[1]));
    for (const needed of [
        "--bk-font-weight-medium",
        "--bk-color-foreground-muted",
        "--bk-font-family-mono",
        "--bk-color-warning",
    ]) {
        assert.ok(
            used.has(needed),
            `knowledge.css no longer reaches ${needed} through the token layer, so whatever replaced it ignores the user's theme`,
        );
    }
});

test("a two-value padding token is never composed into a shorthand", () => {
    /* `--bk-padding-xs|sm|md|lg` are not lengths. Each is `<vertical>
     * <horizontal>` — two values — so they are correct ONLY as the sole value
     * of the `padding` shorthand, where the two halves land where they were
     * meant to. Anywhere else they silently misbehave, and CSS gives no warning
     * for either failure:
     *
     *   padding: var(--bk-padding-md) 0   ->  0.375rem 0.875rem 0
     *                                         a THREE-value shorthand; the
     *                                         bottom padding becomes zero
     *   padding-left: var(--bk-padding-md) ->  a longhand handed two values;
     *                                         the declaration is DROPPED
     *
     * The token guard above cannot catch this: every name involved is real. It
     * is the SHAPE that is wrong, so it needs its own check. `--bk-spacing-*`
     * and `--bk-gap-*` are single lengths and compose freely. */
    const solo = /^\s*var\(--bk-padding-[a-z]+(?:,[^)]*)?\)\s*$/;
    for (const { file, text } of sources()) {
        if (!file.endsWith(".css")) {
            continue;
        }
        /* Comments blanked and the line count kept, because this file's own
         * header has to be able to write out the two failures it is about —
         * and a guard that fails on the explanation of itself is a guard
         * somebody deletes. */
        const declarations = text.replace(/\/\*[\s\S]*?\*\//g, (m) =>
            m.replace(/[^\n]/g, " "),
        );
        declarations.split("\n").forEach((line, i) => {
            const m = line.match(/(padding[a-z-]*)\s*:\s*([^;]*--bk-padding-[^;]*)/);
            if (m === null) {
                return;
            }
            const [, property, value] = m;
            assert.ok(
                property === "padding" && solo.test(value),
                `${file}:${i + 1} composes a two-value padding token into \`${property}: ${value.trim()}\` — ` +
                    `--bk-padding-* is <vertical> <horizontal>, so this either shifts the box or drops the ` +
                    `declaration. Use the single-length --bk-spacing-* for the half you mean.`,
            );
        });
    }
    /* And the check is finding padding rules at all, so it cannot pass on a
     * stylesheet that has stopped using the token. */
    assert.ok(
        /padding:\s*var\(--bk-padding-/.test(stylesheet()),
        "knowledge.css uses no padding token at all; re-read this test before trusting it",
    );
});

test("the stylesheet goes through the token layer and never past it", () => {
    /* Reaching for `--vscode-*` directly couples this surface to the platform
     * rather than to the design system — and then a baukasten that starts
     * mapping a token differently no longer reaches this file. */
    const raw = [...stylesheet().matchAll(/var\(\s*(--vscode-[a-zA-Z0-9-]+)/g)].map((m) => m[1]);
    assert.deepEqual([...new Set(raw)].sort(), []);
});

/* ------------------------------------------- the rail's horizontal overflow */

/** `knowledge.css` as rules, comments stripped and whitespace flattened. */
function rules(): { selector: string; body: string }[] {
    const css = stylesheet().replace(/\/\*[\s\S]*?\*\//g, "");
    const out: { selector: string; body: string }[] = [];
    for (const m of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
        out.push({
            selector: m[1].replace(/\s+/g, " ").trim(),
            body: m[2].replace(/\s+/g, " ").trim(),
        });
    }
    return out;
}

test("the rail holds no Select, so nothing in it has a width floor wider than a sidebar", () => {
    /* baukasten's `Select` puts a 12.5rem min-width on its own root, wider
     * than a VSCode sidebar at its narrowest, and `fullWidth` cannot beat it.
     * The rail had one for the collection scope, and needed a rule on the
     * control to cancel the floor. Opening a collection now scopes the search
     * (index-ui §2), so the rail has no Select — and if one comes back, this
     * says why it needs that cancel again. */
    const sidebar = fs.readFileSync(path.join(ROOT, "webview", "Sidebar.tsx"), "utf8");
    assert.ok(!/<Select\b/.test(sidebar) && !/\bSelect\b.*from "baukasten-ui/.test(sidebar), "a Select is back in the rail: it brings a 12.5rem width floor that must be cancelled on the control");
});

test("the bar stacks when it will not fit, and nothing on the rail clips it", () => {
    /* The other half, and the half that says what SHOULD happen at a width
     * where the two controls genuinely cannot sit side by side: they stack.
     * Without the wrap the field beside the `Select` is squeezed to a few
     * characters instead, which is a narrower kind of broken.
     *
     * AND NOT `overflow: hidden`. Hiding the scrollbar would leave the control
     * overhanging and unreachable, and it would clip an open dropdown with it —
     * a fix for the symptom that breaks something that was working. It is also
     * unnecessary: the `Select` portals its open panel to `document.body`, so
     * there is nothing inside the bar to clip.
     *
     * Named containers only. `.kb-row-title` and `.kb-row-line` clip on purpose
     * — that is §2's clamp — and a blanket scan would fail on them and teach
     * whoever hits it to loosen this test instead. */
    const line = rules().find((r) => r.selector === ".kb-bar");
    assert.ok(line !== undefined, ".kb-bar has no rule");
    assert.match(
        line.body,
        /flex-wrap:\s*wrap/,
        "the bar no longer wraps, so a rail too narrow for both controls squeezes the field instead of stacking",
    );
    const containers = [".kb-view", ".kb-scroll", ".kb-bar", ".kb-search", ".kb-list"];
    for (const r of rules()) {
        const selectors = r.selector.split(",").map((s) => s.trim());
        if (!selectors.some((s) => containers.includes(s))) {
            continue;
        }
        assert.ok(
            !/overflow(-x)?:\s*(hidden|clip)/.test(r.body),
            `${r.selector} clips horizontally; that hides the rail's overflow rather than fixing it, and it would clip an open dropdown too`,
        );
    }
});

test("§2's clamp is a CSS rule as well as a function", () => {
    /* `oneLine` clamps by LENGTH and this clamps by WIDTH, and both are
     * needed: width alone hands the DOM a paragraph per row, length alone
     * leaves a line longer than a 170px rail. */
    const line = rules().find((r) => r.selector === ".kb-row-line");
    assert.ok(line !== undefined, ".kb-row-line has no rule, so the snippet is not clamped at all");
    assert.match(line.body, /overflow:\s*hidden/);
    assert.match(line.body, /text-overflow:\s*ellipsis/);
    assert.match(line.body, /white-space:\s*nowrap/);
});

/* ---------------------------------------- the two list states, in the bundle */

test("the sidebar shows the collections when the query is empty, and a collection's documents inside it, in what ships", () => {
    /* §2: with no query the sidebar shows the collections; a collection opens
     * its documents, with a way back; a query searches. The browse state is
     * not an empty prompt — the property most likely to regress into a "type
     * to search" placeholder — so the branch is pinned in the source, the
     * empty store says §2's sentence, and both reached the bundle. */
    const sidebar = fs.readFileSync(path.join(ROOT, "webview", "Sidebar.tsx"), "utf8");
    assert.match(
        sidebar,
        /isSearching\(settled\)\s*\?\s*\(\s*<SearchList[\s\S]*?\)\s*:\s*place\.kind === "collection"\s*\?\s*\(\s*<CollectionView[\s\S]*?\)\s*:\s*\(\s*<CollectionsList/,
        "the sidebar no longer branches: a query searches, else a collection's documents, else the collections",
    );

    const NOTHING = "Nothing indexed yet. Research lands here when an agent files what it read.";
    assert.ok(sidebar.includes(NOTHING), "§2's empty state is not the sentence §2 writes");

    const text = bundle();
    assert.ok(text.includes(NOTHING), "the shipped bundle does not carry §2's empty state, so the build is stale or the branch is gone");
    assert.ok(text.includes("Load more"), "the shipped lists do not page with Load more");
    assert.ok(text.includes("Manage collections"), "the collections list does not lead to the page where they are renamed and deleted");
    assert.ok(!/type to search/i.test(text), "the shipped bundle prompts the reader to type, which is the empty prompt §2 refuses");
    /* And each state asks the store what it needs. */
    for (const op of ['"collections"', '"ls"', '"search"']) {
        assert.ok(text.includes(op), `the bundle never calls the ${op} operation`);
    }
});

test("a snippet is clamped and never carries markup, in what ships", () => {
    /* §2: "a one-line snippet, clamped, with the full text on hover" and "rows
     * are not highlighted and matches are not marked up". The row renders a
     * STRING, so there is nowhere for a `<mark>` to go — and this checks that
     * the shipped bundle has neither the element nor a highlighter's vocabulary
     * anywhere in it. */
    const text = bundle();
    for (const f of ["<mark", "highlightMatch", "markMatches"]) {
        assert.ok(!text.includes(f), `the shipped bundle contains ${f}, which §2 refuses`);
    }
    /* The clamp and the hover both reached the bundle: the class that clips,
     * and the ellipsis `oneLine` appends when it has cut something. A build
     * that shipped an unclamped row would have the class and not the
     * ellipsis. */
    assert.ok(text.includes("kb-row-line"), "the row's clamped line is not in the bundle");
    /* EITHER SPELLING. esbuild's default charset is ASCII, so a non-ASCII
     * literal reaches the bundle as a `…` escape and not as the
     * character — a check for the character alone would fail on a build that
     * is perfectly correct, which is how a guard gets deleted. */
    assert.ok(
        text.includes("…") || text.includes("\\u2026"),
        "the shipped bundle has no ellipsis in it, so oneLine's length clamp did not survive",
    );
    for (const { file, text: src } of sources()) {
        if (isChecker(file) || !file.startsWith("webview")) {
            continue;
        }
        /* The ELEMENT, terminated, with the prose stripped — `<Markdown` is a
         * component this package renders on purpose and starts with the same
         * five characters, and the files that got this right are the files
         * whose headers have to name the element they are not emitting. */
        assert.ok(
            !/<mark[\s/>]/i.test(code(src)),
            `${file} marks up a match; §2 says rows are not highlighted and matches are not marked up`,
        );
    }
});

/* ------------------------------------------------------ the operation table */

test("every operation the surface can ask is one the host answers", () => {
    /* `protocol.ts` is the vocabulary and `host.ts` is the table. An operation
     * added to one and not the other is a screen that renders nothing and logs
     * nothing. */
    const host = code(fs.readFileSync(path.join(ROOT, "src", "host.ts"), "utf8"));
    const cases = new Set([...host.matchAll(/case "([a-zA-Z]+)":/g)].map((m) => m[1]));
    /* One operation is the `default` branch — a switch over a closed union
     * needs one for the compiler to see it as exhaustive — so the check is
     * that at most one is missing, and that the one missing is named in the
     * default. */
    const missing = OPERATIONS.filter((op) => !cases.has(op));
    assert.ok(
        missing.length <= 1,
        `host.ts answers none of: ${missing.join(", ")}`,
    );
    for (const op of missing) {
        assert.ok(
            host.includes(`${op[0].toUpperCase()}${op.slice(1)}(`) ||
                host.includes(`kb.${op}(`),
            `${op} is in OPERATIONS and host.ts's default branch does not call it either`,
        );
    }
    /* And nothing else: a `case` naming an operation the protocol does not have
     * is dead code that reads as a feature. */
    for (const c of cases) {
        if (["call", "open", "link", "scope", "title", "notify", "addFiles", "addFolder", "init", "settings", "embed", "layoutGet", "layoutPut"].includes(c)) {
            continue; /* the request kinds, which share the file */
        }
        assert.ok(
            (OPERATIONS as readonly string[]).includes(c),
            `host.ts answers "${c}", which is not an operation the protocol has`,
        );
    }
});

test("every mime this package can file is a mime it can render", () => {
    /* `commands.ts` maps VSCode's language ids onto mimes when filing the
     * current file; `view/mime.ts` maps mimes onto §3.2's four renderings. A
     * mime the first can produce and the second cannot place is a source file
     * rendered as prose, which looks like a source file with bad formatting. */
    const commands = fs.readFileSync(path.join(ROOT, "src", "commands.ts"), "utf8");
    const filed = [...commands.matchAll(/^\s+[a-z]+:\s*"([a-z]+\/[a-zA-Z0-9.+-]+)",$/gm)].map(
        (m) => m[1],
    );
    assert.ok(filed.length > 10, `only ${filed.length} mimes were found; the scan is broken`);
    /* Required at module load rather than imported at the top, so this file can
     * say plainly that it is reading the compiled table. */
    const { renderingFor } = require("../view/mime") as { renderingFor(m: string): string };
    for (const mime of filed) {
        /* `text/plain` is the one that is SUPPOSED to render as plain text:
         * §3.2's fourth rendering is "everything else as plain text", and the
         * language id `plaintext` is exactly that. */
        if (mime === "text/plain") {
            continue;
        }
        assert.notEqual(
            renderingFor(mime),
            "text",
            `${mime} is filed by this package and renders as plain text; §3.2 wants markdown, prose or highlighting for it`,
        );
    }
});

test("every mime kb-cli itself can file is one this package can place", () => {
    /* The other half: the CLI guesses a mime from a locator's extension, and
     * its table is the one that decides what is actually in a store. Read out
     * of the C rather than copied, so a mime added there fails here instead of
     * arriving as an unformatted wall of text. The table lives beside the
     * chunker, which a folder's walk and a single `kb add` both ask. */
    const cli = path.resolve(ROOT, "..", "..", "cli", "kb-cli", "src", "chunk.c");
    if (!fs.existsSync(cli)) {
        return;
    }
    const { renderingFor } = require("../view/mime") as { renderingFor(m: string): string };
    const mimes = new Set(
        [...fs.readFileSync(cli, "utf8").matchAll(/"\.[a-z0-9]+",\s*"([a-z]+\/[a-zA-Z0-9.+-]+)"/g)].map(
            (m) => m[1],
        ),
    );
    assert.ok(mimes.size > 8, `only ${mimes.size} mimes were found in kb-cli; the scan is broken`);
    const unplaced = [...mimes].filter((m) => renderingFor(m) === "text" && m !== "text/plain").sort();
    assert.deepEqual(
        unplaced,
        [],
        "kb files these mimes and this package renders them as plain text, so a source document would read as an unformatted wall",
    );
});

/* ------------------------------------------------------------- no writes */

test("nothing in the extension host writes to a store, or anywhere else", () => {
    /* index-ui.md's surface is a READER. Every write it makes goes through the
     * CLI — index-api.md §10 makes the C the only code that writes — so a
     * filesystem call in this package would be a second writer into a store
     * whose content hashes are the whole point.
     *
     * Qualified with `fs.`, so that a METHOD named for a write cannot be
     * mistaken for one. */
    const writes =
        /\bfs\.(writeFileSync|appendFileSync|mkdirSync|rmSync|unlinkSync|renameSync|truncateSync|cpSync|copyFileSync|createWriteStream|promises)\s*[(.]/g;
    const found: string[] = [];
    for (const { file, text } of sources()) {
        if (isChecker(file) || !file.startsWith("src")) {
            continue;
        }
        for (const m of code(text).matchAll(writes)) {
            found.push(`${file}: ${m[1]}`);
        }
        /* `.lap/` and `.coboard/` belong to the tools that own them and this
         * package has no business naming either, anywhere. `.kb/` is named in
         * exactly one place and for a READ — the watcher's glob, checked
         * below — so it is excluded here rather than left to make this
         * assertion unfailable. */
        for (const owned of [".lap", ".coboard"]) {
            assert.ok(
                !new RegExp(`["'\`][^"'\`]*\\${owned}[/\\\\]`).test(code(text)),
                `${file} names a path inside ${owned}/, which belongs to the tool that owns it`,
            );
        }
    }
    assert.deepEqual(
        found,
        [],
        "a filesystem write appeared in the extension host; every write this package makes goes through the CLI",
    );

    /* THE ONE PLACE A STORE PATH IS NAMED, and what it is for. The logs of
     * §1.6 are append-only and there is no daemon to push from, so a watcher
     * over them is what "something changed" means. It is a `RelativePattern`
     * handed to `createFileSystemWatcher`, which cannot write, and it is in one
     * file — a second one would be a second answer about which store this
     * window is looking at. */
    const naming = sources().filter(
        (s) =>
            s.file.startsWith("src") &&
            !isChecker(s.file) &&
            /["'`][^"'`]*\.kb[/\\]/.test(code(s.text)),
    );
    assert.deepEqual(
        naming.map((s) => s.file),
        [path.join("src", "extension.ts")],
        "a second file names a path inside .kb/; the watcher is the only thing here that may",
    );
    const extension = code(fs.readFileSync(path.join(ROOT, "src", "extension.ts"), "utf8"));
    assert.match(
        extension,
        /createFileSystemWatcher\(\s*new vscode\.RelativePattern\(root, "\.kb\/\{documents,sources\}\.jsonl"\)/,
        "the .kb/ path in extension.ts is no longer the watcher's glob over §1.6's two logs",
    );
});

/* ------------------------------------------------------------ packaging */

test("the extension's activation and contributions name what this package builds", () => {
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as Record<
        string,
        any
    >;
    assert.equal(pkg.main, "./out/extension.js");
    assert.ok(fs.existsSync(path.join(ROOT, "out", "extension.js")));

    /* §1: ONE contribution to the activity bar, called Knowledge, with the
     * icon §1 names. §1's table is three columns and each of them is checked:
     * a second container would be a second rail for a surface §1 says is one. */
    const containers = pkg.contributes.viewsContainers.activitybar as {
        id: string;
        title: string;
        icon: string;
    }[];
    assert.equal(containers.length, 1, "index-ui.md §1 asks for one activity-bar contribution");
    assert.equal(containers[0].title, "Knowledge");
    assert.equal(containers[0].icon, "media/knowledge.svg", "§1's table names media/knowledge.svg");
    assert.ok(
        fs.existsSync(path.join(ROOT, containers[0].icon)),
        "the activity-bar icon §1 names is not a file this package ships",
    );

    /* One view in it, and a webview: §1 says a native TreeView cannot carry a
     * search field and per-row metadata. */
    const views = pkg.contributes.views as Record<string, { id: string; type?: string }[]>;
    assert.deepEqual(Object.keys(views), [containers[0].id]);
    assert.equal(views[containers[0].id].length, 1);
    assert.equal(views[containers[0].id][0].type, "webview");
    assert.equal(views[containers[0].id][0].id, "knowledge.documents");

    /* §6: a custom editor on the kb: scheme, with scheme-qualified selectors so
     * no workspace file can ever be routed here. */
    const editors = pkg.contributes.customEditors as {
        viewType: string;
        selector: { filenamePattern: string }[];
    }[];
    assert.equal(editors.length, 1);
    assert.equal(editors[0].viewType, "knowledge.document");
    for (const selector of editors[0].selector) {
        assert.ok(
            selector.filenamePattern.startsWith("kb:"),
            `"${selector.filenamePattern}" could match a file on disk`,
        );
        assert.ok(
            selector.filenamePattern.includes("/"),
            `"${selector.filenamePattern}" has no slash, so VSCode matches it against a basename and it would hijack files`,
        );
    }

    /* §2's three title-bar actions and §5's command, each registered and each
     * on the view's title bar where §2 puts them. */
    const commands = (pkg.contributes.commands as { command: string; title: string }[]).map(
        (c) => c.command,
    );
    for (const needed of [
        "knowledge.addCurrentFile",
        "knowledge.addFolder",
        "knowledge.addUrl",
        "knowledge.refreshStale",
        "knowledge.search",
    ]) {
        assert.ok(commands.includes(needed), `${needed} is not contributed`);
    }
    const titleBar = (pkg.contributes.menus["view/title"] as { command: string; when: string }[])
        .filter((m) => m.when === "view == knowledge.documents")
        .map((m) => m.command);
    for (const needed of ["knowledge.addCurrentFile", "knowledge.addFolder", "knowledge.addUrl", "knowledge.refreshStale"]) {
        assert.ok(titleBar.includes(needed), `${needed} is not on the sidebar's title bar (§2)`);
    }
    /* §5's command is a COMMAND and not a title-bar button: it is for a reader
     * who does not want to leave the keyboard. */
    assert.ok(!titleBar.includes("knowledge.search"));
    const extension = fs.readFileSync(path.join(ROOT, "out", "extension.js"), "utf8");
    for (const needed of commands) {
        assert.ok(
            extension.includes(`"${needed}"`),
            `${needed} is contributed and the compiled extension never registers it, so invoking it fails`,
        );
    }
    assert.ok(
        extension.includes("quickSearch"),
        "the compiled extension does not reach §5's QuickPick",
    );
});

test("the .vsix ships no node_modules", () => {
    /* The host is bundled, so nothing the installed extension runs lives in
     * node_modules. A "!" exception here would bring back the workspace's
     * hoisted tree, which vsce cannot package. */
    const ignore = fs.readFileSync(path.join(ROOT, ".vscodeignore"), "utf8");
    assert.match(ignore, /^node_modules\/\*\*$/m);
    assert.ok(!/^!node_modules/m.test(ignore), ".vscodeignore brings part of node_modules back");
    assert.match(ignore, /^webview\/\*\*$/m);
    assert.match(ignore, /^out\/test\/\*\*$/m);
});

test("knowledge.rerank is a boolean that is off unless the reader turns it on", () => {
    /* It costs seconds a search and needs a model file most machines do not
     * have, so on by default would be every search slow or refused. */
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as {
        contributes: { configuration: { properties: Record<string, { type: string; default: unknown; description: string }> } };
    };
    const setting = pkg.contributes.configuration.properties["knowledge.rerank"];
    assert.ok(setting !== undefined, "package.json does not contribute knowledge.rerank");
    assert.equal(setting.type, "boolean");
    assert.equal(setting.default, false);
    assert.match(setting.description, /gte-reranker-modernbert-base/);
});

test("the extension host is bundled, so the .vsix needs no dependencies", () => {
    /* kb-js is a workspace symlink vsce cannot follow. The bundle inlines it,
     * --no-dependencies stops vsce from walking node_modules, and the built
     * host must not ask for kb-js at runtime. */
    const pkg = JSON.parse(fs.readFileSync(path.join(ROOT, "package.json"), "utf8")) as {
        scripts: Record<string, string>;
    };
    assert.match(pkg.scripts["package"], /--no-dependencies/);
    const script = code(fs.readFileSync(path.join(ROOT, "scripts", "bundle.mjs"), "utf8"));
    assert.ok(script.includes('"extension.js"'), "bundle.mjs does not bundle the extension host");
    assert.ok(script.includes('external: ["vscode"]'), "the host bundle must leave vscode to the editor");
    assert.ok(!fs.existsSync(path.join(ROOT, ".npmrc")), "an .npmrc is back; install-links is no longer how kb-js ships");
    const host = fs.readFileSync(path.join(ROOT, "out", "extension.js"), "utf8");
    assert.ok(!/require\(["']kb-js["']\)/.test(host), "out/extension.js still requires kb-js at runtime");
});

test("the build outputs are gitignored rather than committed", () => {
    /* `out/` is derived from `src/` and the bundle is derived from both;
     * committing either is how a checkout comes to contain a build nobody
     * made. */
    const ignore = fs.readFileSync(path.resolve(ROOT, "..", "..", ".gitignore"), "utf8");
    for (const needed of [
        "packages/kb-js/out/",
        "packages/index-vscode/out/",
        "node_modules/",
    ]) {
        assert.ok(
            ignore.split("\n").includes(needed),
            `.gitignore does not carry ${needed}, so a build would be committed`,
        );
    }
});

/* ------------------------------------------------------------ one store */

test("nothing asks for a tier, because there is one store", () => {
    /* index-api.md §1.4: the knowledge base is the one `.kb/` found by walking
     * up from the working directory. A `store` selector, a global badge or a
     * `KB_STORE` in this package would be vocabulary for a store that no
     * longer exists — and `kb` refuses the option outright. */
    const tiers = /\bstore:\s*["'`]|\balsoGlobal\b|\bdefaultWrite\b|\bKB_STORE\b|\btiers\b|~\/\.kb\b/;
    const found = sources()
        .filter((s) => !s.file.startsWith(path.join("src", "test")) && tiers.test(code(s.text)))
        .map((s) => s.file);
    assert.deepEqual(found, [], "a file still speaks of tiers");
});

test("the client only ever runs in the workspace folder, and not at all without one", () => {
    /* Without a working directory `kb` would walk up from wherever the
     * extension host started and answer about somebody else's store. */
    const session = code(fs.readFileSync(path.join(ROOT, "src", "session.ts"), "utf8"));
    assert.match(
        session,
        /return root === undefined \? undefined : new Kb\(clientOptions\(settings, root\)\)/,
        "makeClient builds a client without a workspace folder to run it in",
    );
    assert.equal(
        sources().filter((s) => !isChecker(s.file) && /new Kb\(/.test(code(s.text))).length,
        1,
        "a second place builds a kb client",
    );
});
