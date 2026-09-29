import { StrictMode } from "react";
import { createRoot } from "react-dom/client";

import favicon from "../../packages/combined/media/procode.svg";
import { App } from "./App";
import "baukasten-ui/dist/baukasten-base.css";
import "baukasten-ui/dist/baukasten-web.css";
import "./style.css";

const link = document.createElement("link");
link.rel = "icon";
link.type = "image/svg+xml";
link.href = favicon;
document.head.append(link);

createRoot(document.getElementById("root")!).render(
    <StrictMode>
        <App />
    </StrictMode>,
);
