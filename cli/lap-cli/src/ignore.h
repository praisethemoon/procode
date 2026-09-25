/* .lapignore handling. Pattern syntax (gitignore subset):
 *   - blank lines and lines starting with '#' are skipped
 *   - trailing '/' restricts the pattern to directories
 *   - a pattern containing '/' is anchored to the repo root
 *     (a leading '/' is equivalent and stripped)
 *   - a pattern without '/' matches the basename at any depth
 *   - '*' matches within a path segment, '?' matches one character,
 *     '**' matches any number of whole segments
 *   - negation ('!') is not supported in this version
 * Built-in defaults: .lap/, .git/, .hg/, .svn/, .DS_Store
 */
#ifndef LAP_IGNORE_H
#define LAP_IGNORE_H

#include "arena.h"

typedef struct Ignore Ignore;

/* Loads <repo_root>/.lapignore (if present) plus built-in defaults. */
Ignore *ignore_load(Arena *a, const char *repo_root);

/* True when relpath (repo-relative, '/' separators) is ignored. For files,
 * parent directories are checked against directory patterns too.
 */
bool ignore_match(const Ignore *ig, const char *relpath, bool is_dir);

/* Single-pattern check, exposed for unit tests. */
bool ignore_match_pattern(const char *pattern, const char *relpath,
                          bool is_dir);

#endif /* LAP_IGNORE_H */
