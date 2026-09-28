/* Starting points for a page, served by the eggzibit_template tool.
 *
 * The report is the first page an agent published, reduced to its shape:
 * every component the viewer styles by class (specs/eggzibit.md §3), with
 * the text replaced by what goes there. It carries no CSS at all — colour,
 * spacing and type come from the viewer — so a page built from it follows
 * the person's theme without trying.
 *
 * Kept as a string in the package, not a file beside it, so the bundled MCP
 * server carries it into every project. */

export interface Template {
    readonly name: string;
    readonly description: string;
    readonly html: string;
}

const REPORT = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>REPORT TITLE</title>
</head>
<body>

<!-- Header: what kind of page and when, the title, one paragraph that says
     what the reader will learn, and the facts that frame it as chips. -->
<div class="eyebrow">Report · DATE</div>
<h1>REPORT TITLE: the finding, not the topic</h1>
<p class="lede">One or two sentences: what was done or examined, and the conclusion.
A reader who stops here should already know the answer.</p>
<div class="meta">
  <span class="chip">Scope <b>WHAT</b></span>
  <span class="chip">Source <b>WHERE THE FACTS CAME FROM</b></span>
  <span class="chip">Status <b>STATE</b></span>
</div>

<!-- Headline numbers: three or four, each with what it counts and what it
     means. Only numbers that were measured. kpi.warn / kpi.danger colour the
     last line. -->
<div class="kpis">
  <div class="kpi"><b>13</b><span>WHAT THIS COUNTS</span><small>WHY IT MATTERS</small></div>
  <div class="kpi"><b>12.6×</b><span>WHAT CHANGED</span><small>FROM → TO</small></div>
  <div class="kpi warn"><b>2</b><span>OPEN QUESTIONS</span><small>see section 4</small></div>
</div>

<nav class="toc">
  <a href="#summary">1. Summary</a>
  <a href="#evidence">2. Evidence</a>
  <a href="#detail">3. Detail</a>
  <a href="#open">4. Open questions</a>
  <a href="#next">5. Next</a>
</nav>

<!-- Summary: findings as callouts, each a verdict in bold and the reason
     under it. ok · info · warn · danger. -->
<h2 id="summary">1. Summary</h2>
<div class="callout ok"><strong>A finding stated as a sentence.</strong><p>The evidence for it, in one or two sentences.</p></div>
<div class="callout info"><strong>Something the reader should know.</strong><p>Context that changes how the rest reads.</p></div>
<div class="callout warn"><strong>A caveat.</strong><p>What is uncertain, and what it would take to settle.</p></div>

<!-- Evidence: a chart beside the table or text that explains it. Charts are
     inline SVG with the chart classes; compute bar widths from the numbers
     and say the scale in the caption. -->
<h2 id="evidence">2. Evidence</h2>
<div class="cols">
  <figure class="panel">
    <svg class="chart" viewBox="0 0 420 150" role="img" aria-label="WHAT THE CHART SHOWS">
      <text x="0" y="14" class="label">CHART TITLE (unit)</text>
      <line class="grid" x1="90" y1="28" x2="90" y2="122"/>
      <line class="grid" x1="250" y1="28" x2="250" y2="122"/>
      <text x="86" y="138">0</text><text x="240" y="138">HALF</text>
      <text x="0" y="46">BEFORE</text>
      <rect class="bar danger" x="90" y="34" width="300" height="18" rx="3"/><text x="396" y="47">7,318</text>
      <text x="0" y="78">MIDDLE</text>
      <rect class="bar warn" x="90" y="66" width="149" height="18" rx="3"/><text x="245" y="79">3,635</text>
      <text x="0" y="110">AFTER</text>
      <rect class="bar ok" x="90" y="98" width="24" height="18" rx="3"/><text x="120" y="111">583</text>
    </svg>
    <figcaption>What was measured, on what, and how. The scale starts at 0.</figcaption>
  </figure>
  <div>
    <table>
      <thead><tr><th>Case</th><th class="num">Value</th><th>Note</th></tr></thead>
      <tbody>
        <tr><td>BEFORE</td><td class="num">7,318</td><td>WHY</td></tr>
        <tr><td>AFTER</td><td class="num">583</td><td>WHY</td></tr>
      </tbody>
    </table>
  </div>
</div>

<!-- Detail: a table the reader can filter. Each row has data-group; each
     button has data-filter ("all" or a group). The script at the end does
     the rest. Use td.id for identifiers and tag for small labels. -->
<h2 id="detail">3. Detail</h2>
<div class="tabs" role="group" aria-label="Filter">
  <button type="button" data-filter="all" aria-pressed="true">All</button>
  <button type="button" data-filter="GROUP-A" aria-pressed="false">GROUP A</button>
  <button type="button" data-filter="GROUP-B" aria-pressed="false">GROUP B</button>
</div>
<table class="filterable">
  <thead><tr><th>Id</th><th>What</th><th>Group</th><th>State</th></tr></thead>
  <tbody>
    <tr data-group="GROUP-A"><td class="id">ID-1</td><td>WHAT HAPPENED</td><td><span class="tag">GROUP A</span></td><td><span class="tag ok">done</span></td></tr>
    <tr data-group="GROUP-B"><td class="id">ID-2</td><td>WHAT HAPPENED</td><td><span class="tag">GROUP B</span></td><td><span class="tag warn">open</span></td></tr>
  </tbody>
</table>
<p class="muted" data-count></p>

<!-- Open questions: what is not verified, each expandable, the most
     important open. Honesty here is what makes the rest believable. -->
<h2 id="open">4. Open questions</h2>
<details open>
  <summary>THE MOST IMPORTANT UNVERIFIED CLAIM</summary>
  What was checked, what was not, and why it matters.
</details>
<details>
  <summary>A SMALLER ONE</summary>
  The same, briefly.
</details>

<!-- Next: what the reader should do, as actions. -->
<h2 id="next">5. Next</h2>
<ul>
  <li>AN ACTION, WITH WHO OR WHAT IT NEEDS.</li>
  <li>ANOTHER.</li>
</ul>

<hr>
<p class="muted">Where the facts come from: the runs, files or documents behind every number above.</p>

<script>
  // Filter buttons over table rows: button[data-filter] × tr[data-group].
  (function () {
    var rows = Array.prototype.slice.call(document.querySelectorAll("table.filterable tbody tr"));
    var buttons = Array.prototype.slice.call(document.querySelectorAll(".tabs button[data-filter]"));
    var count = document.querySelector("[data-count]");
    function show(group) {
      var n = 0;
      rows.forEach(function (r) {
        var on = group === "all" || r.getAttribute("data-group") === group;
        r.style.display = on ? "" : "none";
        if (on) n++;
      });
      buttons.forEach(function (b) { b.setAttribute("aria-pressed", String(b.getAttribute("data-filter") === group)); });
      if (count) count.textContent = n + (n === 1 ? " row" : " rows");
    }
    buttons.forEach(function (b) { b.addEventListener("click", function () { show(b.getAttribute("data-filter")); }); });
    show("all");
  })();
</script>
</body>
</html>
`;

export const TEMPLATES: readonly Template[] = [
    {
        name: "report",
        description:
            "A findings report: header with a lede and chips, headline numbers, callouts, a chart beside a table, a filterable table, open questions, next steps. Uppercase text marks what to replace; the comments say what each part is for — remove them.",
        html: REPORT,
    },
];
