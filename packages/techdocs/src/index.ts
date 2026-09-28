/* techdocs: pages an agent publishes into the workspace (specs/techdocs.md). */

export {
    Techdocs,
    TechdocsError,
    TechdocsErrorCode,
    MAX_DESCRIPTION,
    MAX_KEYWORD,
    MAX_KEYWORDS,
    MAX_PAGE_BYTES,
    MAX_TITLE,
    META,
    PAGE,
    Page,
    PublishInput,
    STORE_DIR,
    defaultRoot,
    findTechdocs,
    hasKeyword,
    isPageId,
    normalizeKeywords,
} from "./store";
export { TEMPLATES, Template } from "./templates";
