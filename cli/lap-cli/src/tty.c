#include "tty.h"

#include "platform.h"

/* SGR codes, indexed by Style. The restraint is the design: colour marks
 * the few things a reader scans for — ids, states, the two sides of a diff
 * — and everything else stays the terminal's own foreground. */
static const char *const CODE[] = {
    "\033[33m",   /* S_ID       yellow    */
    "\033[36m",   /* S_SESSION  cyan      */
    "\033[1;36m", /* S_ACTIVE   bold cyan */
    "\033[32m",   /* S_ADDED    green     */
    "\033[31m",   /* S_REMOVED  red       */
    "\033[36m",   /* S_HUNK     cyan      */
    "\033[33m",   /* S_CHANGED  yellow    */
    "\033[2m",    /* S_MUTED    dim       */
    "\033[1;31m"  /* S_ERROR    bold red  */
};

static bool out_styled;
static bool err_styled;
static bool json_mode;

/* Every value-taking flag in the CLI, as one set.
 *
 * Colour is resolved before a command is chosen, so this scan cannot know
 * which table applies and uses all of them. Being over-broad is the safe
 * direction: the worst case is skipping a token that happened to look like
 * --color=, never misreading one. A message is DATA — `lap commit -i
 * "--color=always"` must neither switch colour on nor be rejected as a bad
 * mode, and before this set existed it did both.
 *
 * Keep in step with the value_flags tables in cmd_*.c; drifting only ever
 * costs a missed flag, never a wrong command. */
static const char *const VALUE_FLAGS[] = {
    "-i",       "--intent", "-b",      "--behavior", "-F",
    "--edit",   "--lines",  "--session", "--file",   "-n",
    "--text",   "--msg",    "--since", "--until",    "--limit",
    "--meta",   NULL};

static bool takes_value(const char *arg) {
    for (int32_t i = 0; VALUE_FLAGS[i]; i++) {
        if (strcmp(arg, VALUE_FLAGS[i]) == 0)
            return true;
    }
    return false;
}

bool tty_init(int32_t argc, char **argv) {
    enum { AUTO = -1, NEVER = 0, ALWAYS = 1 } mode = AUTO;
    bool ok = true;
    json_mode = false;
    for (int32_t i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            break; /* the same grammar every command uses: flags end here */
        if (takes_value(argv[i])) {
            i++; /* that token is data, whatever it looks like */
        } else if (strcmp(argv[i], "--json") == 0) {
            json_mode = true;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            mode = NEVER;
        } else if (strncmp(argv[i], "--color=", sizeof "--color=" - 1) == 0) {
            const char *v = argv[i] + sizeof "--color=" - 1;
            if (strcmp(v, "always") == 0)
                mode = ALWAYS;
            else if (strcmp(v, "never") == 0)
                mode = NEVER;
            else if (strcmp(v, "auto") == 0)
                mode = AUTO;
            else
                ok = false; /* resolve anyway: the complaint wants colour too */
        }
    }
    /* JSON is data, not a display. Gating here rather than in each command's
     * JSON branch makes it structural: a styled helper reached from a JSON
     * path emits nothing, instead of depending on every branch to remember. */
    if (json_mode)
        mode = NEVER;
    /* NO_COLOR is the kill switch, and nothing overrides it: whoever sets
     * it is telling every program in the environment that escapes are
     * unwelcome, which is precisely the promise lap makes to its callers. */
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0])
        mode = NEVER;

    if (mode == ALWAYS) {
        out_styled = err_styled = true;
    } else if (mode == NEVER) {
        out_styled = err_styled = false;
    } else {
        const char *term = getenv("TERM");
        bool dumb = term && strcmp(term, "dumb") == 0;
        out_styled = !dumb && plat_tty_ansi(stdout);
        err_styled = !dumb && plat_tty_ansi(stderr);
    }
    return ok;
}

bool tty_json(void) {
    return json_mode;
}

static bool styled(FILE *f) {
    return f == stderr ? err_styled : out_styled;
}

const char *sgr_f(FILE *f, Style s) {
    return styled(f) ? CODE[s] : "";
}

const char *sgr_off_f(FILE *f) {
    return styled(f) ? "\033[0m" : "";
}

size_t sb_text(StrBuf *sb, const char *s, size_t n) {
    size_t cols = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t' || (c >= 0x20 && c != 0x7f)) {
            sb_putc(sb, (char)c);
            cols++;
        } else {
            /* caret notation, as a pager shows it: visible, and two columns
             * wide rather than an invisible byte that silently shifts a
             * table */
            sb_putc(sb, '^');
            sb_putc(sb, c == 0x7f ? '?' : (char)(c + '@'));
            cols += 2;
        }
    }
    return cols;
}

void sb_pad_text(StrBuf *sb, const char *s, int32_t width) {
    size_t cols = sb_text(sb, s, strlen(s));
    for (int32_t pad = width - (int32_t)cols; pad > 0; pad--)
        sb_putc(sb, ' ');
}

void sb_field(StrBuf *sb, Style s, const char *text, int32_t width) {
    size_t cols = 0;
    if (*text) {
        sb_puts(sb, sgr(s));
        cols = sb_text(sb, text, strlen(text));
        sb_puts(sb, sgr_off());
    }
    for (int32_t pad = width - (int32_t)cols; pad > 0; pad--)
        sb_putc(sb, ' ');
}
