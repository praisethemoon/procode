/* .gitignore, as git reads it, for `kb add --dir`: a folder is filed the way
 * the repository it belongs to sees it, so what git ignores (build output,
 * dependencies, caches) is not filed either.
 *
 * THE RULES ARE GIT'S (gitignore(5)), all of them, because a subset silently
 * files what the user excluded:
 *
 *   - blank lines and `#` lines are skipped; `\#` and `\!` are literal; trailing
 *     spaces are dropped unless escaped;
 *   - `!` re-includes what an earlier pattern excluded — but nothing under an
 *     excluded directory, which the walk never enters;
 *   - a trailing `/` matches directories only;
 *   - a pattern with a `/` at the start or in the middle is anchored to the
 *     directory of its .gitignore; without one it matches a name at any depth
 *     below it;
 *   - `*` and `?` never match `/`; `[...]` is a class (`!` or `^` negates);
 *     `**` as a whole segment matches any number of directories;
 *   - the LAST matching pattern decides, and a deeper .gitignore is read after
 *     the ones above it, so it overrides them.
 *
 * Paths are relative to the TOP: the repository's root (the nearest directory
 * at or above the walked one that holds `.git`), or the walked directory when
 * it is in no repository. The .gitignore files between the top and the walked
 * directory apply too, and so does `.git/info/exclude`. The user's global
 * excludes file is not read: kb does not look in the home directory.
 */
#ifndef KB_GITIGNORE_H
#define KB_GITIGNORE_H

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct GitIgnore GitIgnore;

GitIgnore *gitignore_new(Arena *a);

/* Adds the patterns in `text`, read from the .gitignore of directory `base`
 * (relative to the top, "" for the top itself). */
void gitignore_add(GitIgnore *g, const char *base, const char *text, size_t len);

/* Reads <top>/<base>/.gitignore when there is one. */
void gitignore_load(GitIgnore *g, const char *top, const char *base);

/* Whether `path` (relative to the top, `/`-separated) is ignored by the
 * patterns added so far. Its parent directories are not checked: a walk does
 * not enter an ignored directory. */
bool gitignore_match(const GitIgnore *g, const char *path, bool is_dir);

/* One glob against one path, git's wildmatch with `*` stopping at `/`.
 * Exposed for tests. */
bool gitignore_wildmatch(const char *pattern, const char *path);

#endif /* KB_GITIGNORE_H */
