/* Terminal styling.
 *
 * Colour is a courtesy to a human at a terminal, never part of the output.
 * Agents read lap through pipes, so a stream that is not a terminal gets
 * exactly the bytes it would have got if this file did not exist, and no
 * layout differs between the two modes — only escape sequences, which
 * occupy no columns, are added.
 *
 * The mode is a property of the process, like the locale: resolved once
 * from the command line and the environment, then read everywhere. Every
 * accessor returns "" when styling is off, so call sites read the same in
 * both modes and there is no second code path to keep in step.
 */
#ifndef LAP_TTY_H
#define LAP_TTY_H

#include "str.h"

typedef enum {
    S_ID,      /* commit ids                   */
    S_SESSION, /* session ids                  */
    S_ACTIVE,  /* the session currently open   */
    S_ADDED,   /* added lines, new files       */
    S_REMOVED, /* removed lines, deleted files */
    S_HUNK,    /* @@ range headers             */
    S_CHANGED, /* modified files               */
    S_MUTED,   /* timestamps, ranges, asides   */
    S_ERROR    /* the error: prefix            */
} Style;

/* Resolves the mode from --color=<auto|always|never>, --no-color, $NO_COLOR,
 * $TERM and isatty(). Called once, from main; returns false when --color
 * names a mode that does not exist. */
bool tty_init(int32_t argc, char **argv);

/* True when --json was given. Resolved by the same scan, because a
 * diagnostic raised before a command is chosen still has to answer in the
 * shape its caller asked for. */
bool tty_json(void);

/* Style prefix and reset for one stream, "" when that stream is unstyled.
 * stdout and stderr are gated separately: `lap log | less` must still show
 * a human the diagnostic on their terminal in red. */
const char *sgr_f(FILE *f, Style s);
const char *sgr_off_f(FILE *f);

static inline const char *sgr(Style s) { return sgr_f(stdout, s); }
static inline const char *sgr_off(void) { return sgr_off_f(stdout); }

/* Appends text styled for stdout, then pads with spaces to `width` VISIBLE
 * columns. Escape sequences occupy no columns, which is exactly what
 * printf's "%-*s" cannot know — so every aligned column goes through here,
 * and the layout comes out identical whether or not colour is on. */
void sb_field(StrBuf *sb, Style s, const char *text, int32_t width);

#endif /* LAP_TTY_H */
