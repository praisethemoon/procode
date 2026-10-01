/* A step's body and an option's description, as Markdown.
 *
 * react-markdown with remark-gfm and no raw-HTML plugin, as the Board and
 * Knowledge render it: HTML in the Markdown is shown as text, never as markup.
 * HTML belongs in an option's preview, which is sandboxed. Links keep
 * react-markdown's default URL check (http, https, mailto); VS Code opens them
 * outside the webview.
 */

import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";

const PLUGINS = [remarkGfm];

export function Markdown(props: { source: string }): JSX.Element {
    return (
        <div className="ask-prose">
            <ReactMarkdown remarkPlugins={PLUGINS}>{props.source}</ReactMarkdown>
        </div>
    );
}
