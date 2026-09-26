/* Which files under a folder `kb add --dir` files, and why each of the others
 * is left out. Reading only: the scan decides, cmd_add writes.
 *
 * A FILE IS FILED when all of these hold:
 *   - git would track it: no .gitignore (§ gitignore.h) excludes it or a
 *     directory above it;
 *   - it is not hidden (no path segment starts with `.`), which keeps out
 *     .git, .kb, .lap, editor state and dotfile configuration;
 *   - it is not under a directory that holds someone else's code:
 *     node_modules, vendor, third_party, bower_components, __pycache__,
 *     venv, site-packages;
 *   - its name says source or documentation (kb_mime_from_path knows the
 *     extension, or it is a Makefile, Dockerfile or the like);
 *   - it is not generated: no lock file, no minified bundle, and no
 *     `@generated` or "DO NOT EDIT" marker near its top;
 *   - it is text: valid UTF-8 with no NUL byte;
 *   - it is at most KB_DIR_MAX_BYTES (JSON at most KB_DIR_MAX_JSON_BYTES:
 *     past that it is data, not configuration).
 *
 * Files come out in the walk's order, which is sorted, so two scans of the
 * same tree file it in the same order and give it the same ids.
 */
#ifndef KB_DIRSCAN_H
#define KB_DIRSCAN_H

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KB_DIR_MAX_BYTES (1024u * 1024u)
#define KB_DIR_MAX_JSON_BYTES (64u * 1024u)

typedef struct {
    const char *rel;  /* relative to the scanned folder, `/`-separated */
    const char *mime;
    const char *content;
    size_t len;
} DirFile;

typedef struct {
    DirFile *v;
    size_t n, cap;
    /* What was left out, by reason. */
    size_t ignored;   /* .gitignore */
    size_t hidden;    /* a `.` segment */
    size_t vendored;  /* someone else's code */
    size_t other;     /* not a type kb files from a folder */
    size_t generated;
    size_t binary;
    size_t large;
    size_t unreadable;
    /* The repository root the .gitignore paths are relative to, or the
     * folder itself outside a repository. */
    const char *top;
} DirScan;

/* Scans `root`, an absolute path to a directory. */
bool dir_scan(Arena *a, const char *root, DirScan *out, char *err, size_t errsz);

/* The file-name half of the rules, exposed for tests: whether a name (the
 * last path segment) is generated, and the mime a folder file is filed as
 * (NULL when kb does not file that kind of file from a folder). */
bool dir_name_generated(const char *name);
const char *dir_file_mime(const char *name);

/* Whether the first bytes of a text carry a generated-code marker. */
bool dir_text_generated(const char *text, size_t len);

#endif /* KB_DIRSCAN_H */
