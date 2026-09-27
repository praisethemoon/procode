/* A board tab: one item, rendered by the view for its kind. The host sends the
 * item's view whenever the board changes — including when an agent changed it
 * — and the tab re-renders from that. */

import { StrictMode, useEffect, useState } from "react";
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

    useEffect(() => {
        const stop = listen((m) => {
            if (m.type === "data") {
                setData(m);
                setError(null);
            } else if (m.type === "sessions") {
                setSessions(m.sessions);
            } else if (m.type === "commits") {
                setCommits((c) => ({ ...c, [m.session]: m.error ?? m.commits }));
            } else if (m.type === "error") {
                setError(m.message);
            }
        });
        send({ type: "ready" });
        return stop;
    }, []);

    if (!data) {
        return <p className="cb-muted">Loading {props.id}…</p>;
    }
    const choices: Choices = data.choices;
    const v = data.view;
    return (
        <main className="cb-page">
            {error && (
                <div className="cb-error" onClick={() => setError(null)} title="Dismiss">
                    {error}
                </div>
            )}
            {v === null ? (
                <p className="cb-muted">{props.id} is no longer on the board.</p>
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
