/* A board tab: one item, rendered by the view for its kind. The host sends the
 * item's view whenever the board changes — including when an agent changed it
 * — and the tab re-renders from that. A link followed in the tab is a view of
 * another item, and the tab becomes that item's. */

import { StrictMode, useEffect, useRef, useState } from "react";
import { createRoot } from "react-dom/client";

import type { LapCommit } from "coboard/lap";
import type { Choices, Sessions, ToView } from "../src/protocol";
import { listen, send } from "./rpc";
import { Review } from "./review";
import { Epic, Milestone, Ticket } from "./views";

type Data = Extract<ToView, { type: "data" }>;
type ReviewData = Extract<ToView, { type: "review" }>;

/* A session's review tab: one message, drawn once. */
function ReviewApp(props: { session: string }): JSX.Element {
    const [data, setData] = useState<ReviewData | null>(null);
    useEffect(() => {
        const stop = listen((m) => {
            if (m.type === "review") setData(m);
        });
        send({ type: "ready" });
        return stop;
    }, []);
    return (
        <main className="cb-page">
            {data === null ? (
                <p className="cb-muted">Reading {props.session} from lap…</p>
            ) : (
                <Review
                    session={data.session}
                    ticket={data.ticket}
                    review={data.review}
                    error={data.error}
                    branch={data.branch}
                    adoptedFrom={data.adoptedFrom}
                    stops={data.stops}
                />
            )}
        </main>
    );
}

function App(props: { id: string }): JSX.Element {
    const [data, setData] = useState<Data | null>(null);
    const [sessions, setSessions] = useState<Sessions | null>(null);
    const [commits, setCommits] = useState<Record<string, readonly LapCommit[] | string>>({});
    const [error, setError] = useState<string | null>(null);
    /* The item shown; a ticket's sessions that arrive after the tab left it
     * are dropped. */
    const shown = useRef(props.id);

    useEffect(() => {
        const stop = listen((m) => {
            if (m.type === "data") {
                if (m.id !== shown.current) {
                    shown.current = m.id;
                    setSessions(null);
                    setCommits({});
                }
                setData(m);
                setError(null);
            } else if (m.type === "sessions") {
                if (m.ticket === shown.current) setSessions(m.sessions);
            } else if (m.type === "commits") {
                setCommits((c) => ({ ...c, [m.session]: m.error ?? m.commits }));
            } else if (m.type === "error") {
                setError(m.message);
            }
        });
        /* The mouse's back and forward buttons, as in a browser. */
        const side = (e: MouseEvent) => {
            if (e.button !== 3 && e.button !== 4) return;
            e.preventDefault();
            send({ type: "history", go: e.button === 3 ? "back" : "forward" });
        };
        window.addEventListener("mouseup", side);
        send({ type: "ready" });
        return () => {
            stop();
            window.removeEventListener("mouseup", side);
        };
    }, []);

    const at = data?.id;
    useEffect(() => {
        window.scrollTo(0, 0);
    }, [at]);

    if (!data) {
        return <p className="cb-muted">Loading {props.id}…</p>;
    }
    const choices: Choices = data.choices;
    const v = data.view;
    return (
        /* keyed by the item, so what a view holds for one (a draft, an open
         * picker) is not carried to the next */
        <main className="cb-page" key={data.id}>
            {error && (
                <div className="cb-error" onClick={() => setError(null)} title="Dismiss">
                    {error}
                </div>
            )}
            {v === null ? (
                <p className="cb-muted">{data.id} is no longer on the board.</p>
            ) : v.kind === "epic" ? (
                <Epic v={v} mode={data.mode} />
            ) : v.kind === "milestone" ? (
                <Milestone v={v} choices={choices} mode={data.mode} />
            ) : (
                <Ticket v={v} choices={choices} sessions={sessions} commits={commits} />
            )}
        </main>
    );
}

const root = document.getElementById("root")!;
const id = root.dataset["id"] ?? "";
createRoot(root).render(
    <StrictMode>{id.startsWith("review:") ? <ReviewApp session={id.slice("review:".length)} /> : <App id={id} />}</StrictMode>,
);
