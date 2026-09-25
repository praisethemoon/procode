/* The pieces every view is built from.
 *
 * `useQuery` IS THE WHOLE OF THE DATA LAYER: an operation, its input, and a
 * re-ask whenever the store moves. There is nothing to subscribe to — `kb` is a
 * process that starts and exits — so "something changed" is all the host can
 * say and every view re-asks its own question.
 *
 * A REFUSAL AND A FAULT ARE DRAWN DIFFERENTLY, and that is index-api.md §10's
 * three exit codes reaching the screen. `Refused` shows §11's code and message,
 * because a reader looking at `model_mismatch` is looking at a store that owes
 * a reindex and the word is what they can act on. `Faulted` says kb failed and
 * that there is nothing to correct in what was asked.
 */

import { useCallback, useEffect, useRef, useState } from "react";
import { Icon, Spinner } from "baukasten-ui/core";

import { Operation, WireError } from "../src/protocol";
import { HostFault, StoreRefusal, call, onHostEvent } from "./rpc";

/* A codicon name is a string as far as this file is concerned; baukasten's
 * `Icon` types it against its own union. */
export function Codicon(props: { name: string; className?: string }): JSX.Element {
    const Any = Icon as unknown as (p: Record<string, unknown>) => JSX.Element;
    return <Any name={props.name} className={props.className} />;
}

export type QueryState<T> =
    | { status: "loading" }
    | { status: "ok"; value: T }
    | { status: "refused"; error: WireError }
    | { status: "faulted"; message: string };

/* The input is compared by its JSON rather than by identity, so a caller may
 * build it inline — which is what every call site wants to do — without
 * re-asking on every render. */
export function useQuery<T>(
    op: Operation,
    input: unknown = {},
): { state: QueryState<T>; refresh: () => void } {
    const key = JSON.stringify(input);
    const [state, setState] = useState<QueryState<T>>({ status: "loading" });
    const [nonce, setNonce] = useState(0);
    useEffect(() => {
        let cancelled = false;
        call<T>(op, JSON.parse(key) as unknown)
            .then((value) => {
                if (!cancelled) {
                    setState({ status: "ok", value });
                }
            })
            .catch((e: unknown) => {
                if (cancelled) {
                    return;
                }
                if (e instanceof StoreRefusal) {
                    setState({ status: "refused", error: e.error });
                } else if (e instanceof HostFault) {
                    setState({ status: "faulted", message: e.message });
                } else {
                    setState({ status: "faulted", message: String(e) });
                }
            });
        return () => {
            cancelled = true;
        };
    }, [op, key, nonce]);
    const refresh = useCallback(() => setNonce((n) => n + 1), []);
    useEffect(
        () =>
            onHostEvent((r) => {
                if (r.kind === "changed") {
                    refresh();
                }
            }),
        [refresh],
    );
    return { state, refresh };
}

/* §2's debounce: "search runs debounced as the reader types; a local hybrid
 * search is fast enough that submitting is unnecessary ceremony."
 *
 * IT IS A HOOK AND NOT A TIMER INSIDE A HANDLER because the value it produces
 * is what the query is keyed on — so the query re-runs when the settled value
 * changes and not when a keystroke arrives. A timer that called the query
 * directly would race with React's own state and would fire once more after
 * the component went away. */
export function useDebounced<T>(value: T, ms: number): T {
    const [settled, setSettled] = useState(value);
    useEffect(() => {
        const timer = setTimeout(() => setSettled(value), ms);
        return () => clearTimeout(timer);
    }, [value, ms]);
    return settled;
}

export function Refused(props: { error: WireError }): JSX.Element {
    const { error } = props;
    return (
        <div className="kb-notice">
            <h2>{error.message}</h2>
            <div className="kb-muted kb-small">
                {error.code}
                {error.unrecognised
                    ? " — a code this extension has not been told about, which means kb has moved on without it"
                    : ""}
            </div>
        </div>
    );
}

export function Faulted(props: { message: string }): JSX.Element {
    return (
        <div className="kb-notice">
            <h2>kb failed.</h2>
            <div className="kb-small">
                This is not a refusal from the store — it is a fault, and there is nothing to
                correct in what was asked.
            </div>
            <pre className="kb-pre">{props.message}</pre>
        </div>
    );
}

export function Loading(props: { what: string }): JSX.Element {
    return (
        <div className="kb-empty kb-spread">
            <Spinner size="sm" />
            <span>{props.what}</span>
        </div>
    );
}

/* The one place a QueryState becomes elements, so no view has its own opinion
 * about what a failure looks like. */
export function Resolved<T>(props: {
    state: QueryState<T>;
    loading: string;
    children: (value: T) => JSX.Element;
}): JSX.Element {
    const { state } = props;
    if (state.status === "loading") {
        return <Loading what={props.loading} />;
    }
    if (state.status === "refused") {
        return <Refused error={state.error} />;
    }
    if (state.status === "faulted") {
        return <Faulted message={state.message} />;
    }
    return props.children(state.value);
}

/* §2 and §3.1's badge. One component, because it appears on a row and in a
 * header and the two must not be able to disagree about what stale looks
 * like. */
export function StaleBadge(props: { title?: string }): JSX.Element {
    return (
        <span
            className="kb-stale"
            title={
                props.title ??
                "Older than the staleness threshold. Documentation moves, and a passage that cannot say how old it is will eventually be believed when it should not be."
            }
        >
            <Codicon name="warning" />
            stale
        </span>
    );
}

/* A focusable element reached by a ref, for §3.2's scroll-to-heading. Kept here
 * because both the document body and the sidebar's list need the same rule:
 * scrolling something into view must not steal focus from the box the reader
 * is typing in. */
export function useScrollIntoView(active: boolean): (el: HTMLElement | null) => void {
    const done = useRef(false);
    return useCallback(
        (el: HTMLElement | null) => {
            if (el === null || !active || done.current) {
                return;
            }
            done.current = true;
            el.scrollIntoView({ block: "start" });
        },
        [active],
    );
}
