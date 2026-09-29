import { useEffect, useState } from "react";
import { Badge, Button, Heading, Icon, Link, Text } from "baukasten-ui/core";
import type { CodiconName } from "baukasten-ui/core";
import { Menu, MenuItem } from "baukasten-ui/extra";

import logo from "../../packages/combined/media/procode.svg";
import { SECTIONS } from "./sections";

const REPO = "https://github.com/praisethemoon/procode";

const ICONS: Record<string, CodiconName> = {
    intro: "home",
    lap: "history",
    kb: "book",
    coboard: "project",
    "lap-history": "git-commit",
    knowledge: "library",
    techdocs: "note",
    commands: "terminal",
    security: "shield",
    faq: "question",
};

/* The chapter being read: the last section whose top has passed a line a
 * little below the top of the window. Scroll position rather than
 * IntersectionObserver, because a long section is the current one for as
 * long as it fills the screen, even with no heading in view. */
function useCurrent(ids: readonly string[]): string {
    const [current, setCurrent] = useState(ids[0]);
    useEffect(() => {
        const update = () => {
            const line = window.innerHeight * 0.25;
            let at = ids[0];
            for (const id of ids) {
                const el = document.getElementById(id);
                if (el && el.getBoundingClientRect().top <= line) at = id;
            }
            /* the last chapter may be too short to ever reach the line */
            if (window.innerHeight + window.scrollY >= document.body.scrollHeight - 2) at = ids[ids.length - 1];
            setCurrent(at);
        };
        update();
        window.addEventListener("scroll", update, { passive: true });
        window.addEventListener("resize", update);
        return () => {
            window.removeEventListener("scroll", update);
            window.removeEventListener("resize", update);
        };
    }, [ids]);
    return current;
}

const IDS = SECTIONS.map((s) => s.id);

function go(id: string, behavior: ScrollBehavior = "smooth") {
    document.getElementById(id)?.scrollIntoView({ behavior });
    history.replaceState(null, "", `#${id}`);
}

export function App() {
    const current = useCurrent(IDS);
    const [open, setOpen] = useState(false);

    /* A link to a chapter (/#security) arrives before React has drawn the
     * chapters, so the browser finds nothing to scroll to. */
    useEffect(() => {
        const id = decodeURIComponent(window.location.hash.slice(1));
        if (id) go(id, "instant");
    }, []);

    return (
        <div className="layout">
            <aside className={open ? "sidebar open" : "sidebar"}>
                <div className="brand">
                    <a href="#intro" className="brand-link" onClick={() => setOpen(false)}>
                        <img src={logo} alt="" width={28} height={28} />
                        <Text size="lg" weight="semibold">
                            procode
                        </Text>
                        <Badge variant="info" size="sm">
                            Preview
                        </Badge>
                    </a>
                    <Button
                        className="menu-toggle"
                        variant="secondary"
                        size="sm"
                        aria-expanded={open}
                        aria-controls="chapters"
                        onClick={() => setOpen(!open)}
                    >
                        <Icon name={open ? "close" : "menu"} /> Chapters
                    </Button>
                </div>
                <nav id="chapters" aria-label="Chapters">
                    <Menu>
                        {SECTIONS.map((s) => (
                            <MenuItem
                                key={s.id}
                                icon={<Icon name={ICONS[s.id] ?? "circle-small"} />}
                                selected={current === s.id}
                                aria-current={current === s.id ? "location" : undefined}
                                onClick={() => {
                                    setOpen(false);
                                    go(s.id);
                                }}
                            >
                                {s.title}
                            </MenuItem>
                        ))}
                    </Menu>
                    <div className="side-links">
                        <Link href={REPO} variant="muted">
                            <Icon name="github" /> GitHub
                        </Link>
                        <Link href={`${REPO}/releases`} variant="muted">
                            <Icon name="package" /> Releases
                        </Link>
                    </div>
                </nav>
            </aside>
            <main>
                {SECTIONS.map((s) => (
                    <section key={s.id} id={s.id} aria-labelledby={`${s.id}-title`}>
                        <Heading level={s.id === "intro" ? 1 : 2} id={`${s.id}-title`} marginTop={false}>
                            {s.heading ?? s.title}
                        </Heading>
                        {s.body}
                    </section>
                ))}
                <footer>
                    <Text size="sm" color="muted">
                        MIT licensed. Source, issues and releases on <Link href={REPO}>GitHub</Link>. Built with{" "}
                        <Link href="https://github.com/TypeFox/baukasten">baukasten</Link>.
                    </Text>
                </footer>
            </main>
        </div>
    );
}
