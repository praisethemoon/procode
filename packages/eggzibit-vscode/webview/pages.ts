/* The pages view: the filter bar the Board and Lap History have, then a row
 * per page — its id, title and when it changed; its description; its
 * keywords, each a button that filters by it.
 *
 * Plain DOM, no framework: the view is one bar and a list. Every piece of a
 * page reaches the document through textContent, never as markup, since an
 * agent wrote it. What is typed or chosen survives the view being hidden and
 * VS Code restarting (the webview's own state). */

import { EMPTY, PageFilter, isActive, keywordCounts, matches, toggleKeyword } from "../src/filter";
import { describeWhen } from "../src/list";
import type { PageRow, ToHost, ToView } from "../src/protocol";

declare function acquireVsCodeApi(): {
    postMessage(m: unknown): void;
    getState(): unknown;
    setState(s: unknown): void;
};

const vscode = acquireVsCodeApi();
const send = (m: ToHost) => vscode.postMessage(m);

let pages: readonly PageRow[] = [];
let hasFolder = true;
let loaded = false;
let open = false;
let filter: PageFilter = restore();

function restore(): PageFilter {
    const s = vscode.getState() as { filter?: Partial<PageFilter>; open?: boolean } | undefined;
    open = s?.open === true;
    const f = s?.filter;
    return f && typeof f.text === "string" && Array.isArray(f.keywords)
        ? { text: f.text, keywords: f.keywords.filter((k): k is string => typeof k === "string") }
        : EMPTY;
}

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string): HTMLElementTagNameMap[K] {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
}

function icon(name: string): HTMLElement {
    const i = el("i", `codicon codicon-${name}`);
    i.setAttribute("aria-hidden", "true");
    return i;
}

function button(cls: string, label: string): HTMLButtonElement {
    const b = el("button", cls);
    b.type = "button";
    b.title = label;
    b.setAttribute("aria-label", label);
    return b;
}

/* ------------------------------------------------------------- the bar */

const root = document.getElementById("root")!;
const bar = el("div", "pg-filter");
const line = el("div", "pg-bar");
const chevron = button("pg-chevron", "Filter by keyword");
const input = el("input", "pg-input");
input.type = "text";
input.placeholder = "Filter by id, title, description or keyword";
input.spellcheck = false;
input.setAttribute("aria-label", "Filter the pages");
input.value = filter.text;
const clear = button("pg-clear", "Clear the filter");
clear.append(icon("close"));
line.append(chevron, input, clear);
const chips = el("div", "pg-chips");
bar.append(line, chips);
const list = el("div", "pg-list");
list.setAttribute("role", "list");
root.append(bar, list);

chevron.onclick = () => {
    open = !open;
    render();
};
input.oninput = () => set({ ...filter, text: input.value });
input.onkeydown = (e) => {
    if (e.key === "Escape" && input.value !== "") {
        e.preventDefault();
        input.value = "";
        set({ ...filter, text: "" });
    }
};
clear.onclick = () => {
    input.value = "";
    set(EMPTY);
    input.focus();
};

function set(f: PageFilter): void {
    filter = f;
    render();
}

/* The keywords to choose from: those the pages carry, and any still chosen
 * that no page carries any more, so it can be switched off. */
function choices(): { keyword: string; count: number }[] {
    const counts = keywordCounts(pages);
    for (const k of filter.keywords) if (!counts.some((c) => c.keyword === k)) counts.push({ keyword: k, count: 0 });
    return counts;
}

function render(): void {
    vscode.setState({ filter, open });
    const all = choices();
    chevron.replaceChildren(icon(open ? "chevron-down" : "chevron-right"));
    chevron.setAttribute("aria-expanded", String(open));
    chevron.title = open ? "Hide the keywords" : "Filter by keyword";
    if (filter.keywords.length) chevron.append(el("span", "pg-count", String(filter.keywords.length)));
    clear.hidden = !isActive(filter);
    chips.hidden = !open;
    chips.replaceChildren(
        ...(all.length ? all.map(chip) : [el("span", "pg-none", "No page carries a keyword yet.")]),
    );
    list.replaceChildren(...rows());
}

function chip(c: { keyword: string; count: number }): HTMLElement {
    const on = filter.keywords.includes(c.keyword);
    const b = el("button", `pg-chip${on ? " pg-on" : ""}${c.count === 0 ? " pg-gone" : ""}`);
    b.type = "button";
    b.setAttribute("aria-pressed", String(on));
    b.title = `${c.count} page${c.count === 1 ? "" : "s"}`;
    if (on) b.append(icon("check"));
    b.append(document.createTextNode(c.keyword), el("span", "pg-chip-n", String(c.count)));
    b.onclick = () => set(toggleKeyword(filter, c.keyword));
    return b;
}

/* ------------------------------------------------------------ the rows */

function rows(): HTMLElement[] {
    if (!hasFolder) return [el("p", "pg-empty", "Open a folder to see its pages.")];
    if (!loaded) return [el("p", "pg-empty", "Reading the pages…")];
    if (pages.length === 0) {
        return [
            el(
                "p",
                "pg-empty",
                "No pages yet. An agent publishes one with the eggzibit_publish tool of the eggzibit MCP server: an HTML page with a title, a description and keywords, saved under .eggzibit/.",
            ),
        ];
    }
    const shown = pages.filter((p) => matches(p, filter));
    if (shown.length === 0) return [el("p", "pg-empty", "Nothing matches the filter.")];
    const now = Date.now();
    return shown.map((p) => row(p, now));
}

function row(p: PageRow, now: number): HTMLElement {
    const r = el("div", "pg-row");
    r.tabIndex = 0;
    r.setAttribute("role", "listitem");
    r.dataset.vscodeContext = JSON.stringify({ webviewSection: "page", id: p.id, preventDefaultContextMenuItems: true });
    r.title = `${p.id} — ${p.title}${p.description ? `\n${p.description}` : ""}\ncreated ${p.createdAt} · updated ${p.updatedAt}`;
    const head = el("div", "pg-head");
    const source = button("pg-action", "Open HTML Source");
    source.append(icon("code"));
    source.onclick = (e) => {
        e.stopPropagation();
        send({ type: "source", id: p.id });
    };
    head.append(icon("preview"), el("span", "pg-id", p.id), el("span", "pg-title", p.title), el("span", "pg-when", describeWhen(p.updatedAt, now)), source);
    r.append(head);
    if (p.description) r.append(el("div", "pg-desc", p.description));
    if (p.keywords.length) {
        const k = el("div", "pg-keywords");
        for (const w of p.keywords) {
            const c = el("button", `pg-kw${filter.keywords.includes(w) ? " pg-on" : ""}`, w);
            c.type = "button";
            c.title = `Only the pages about ${w}`;
            c.onclick = (e) => {
                e.stopPropagation();
                set(toggleKeyword(filter, w));
            };
            k.append(c);
        }
        r.append(k);
    }
    r.onclick = () => send({ type: "open", id: p.id });
    r.onkeydown = (e) => {
        if (e.key === "Enter" && e.target === r) send({ type: "open", id: p.id });
    };
    return r;
}

window.addEventListener("message", (e: MessageEvent) => {
    const m = e.data as ToView;
    if (m?.type === "pages") {
        pages = m.pages;
        hasFolder = m.hasFolder;
        loaded = true;
        render();
    }
});
render();
send({ type: "ready" });
