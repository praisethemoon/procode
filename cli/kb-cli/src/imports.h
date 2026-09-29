/* `imports` links (§6): each file of a folder filed with `kb add --dir`
 * linked to the files of that folder it imports or includes.
 *
 * KB'S OWN, NEVER BY HAND. The code says what a file imports, so filing the
 * folder again is what keeps the links true: each filing works out the
 * whole set its files call for and writes only the difference, a `link` for
 * an edge that is new and an `unlink` for one that is gone. A hand-made link
 * of any other type is never touched, and `kb links` refuses to make or
 * remove an imports link itself.
 *
 * ONLY THE FOLDER'S OWN FILES. An import that does not resolve to a file of
 * the same folder source — the standard library, a system header, a
 * package under node_modules — makes no link and is not recorded as
 * unresolved: a graph of what the folder is made of, not of the world.
 */
#ifndef KB_IMPORTS_H
#define KB_IMPORTS_H

#include "dirscan.h"
#include "store.h"
#include "syntax.h"

/* Whether `path` (relative to the folder, '/'-separated) is a filed file. */
typedef bool (*ImportExists)(void *ud, const char *path);

/* The folder file an import names, or NULL:
 *
 *   C       "x.h" beside the including file, else from the folder's root;
 *           <x.h> from the root, else beside the file
 *   TS/JS   a relative specifier ("./x", "../x"), as written, then with
 *           .ts .tsx .d.ts .js .jsx .mjs .cjs .mts .cts, then as a folder's
 *           index.*; a ".js" specifier also finds the .ts it compiles from
 *   Python  relative (.m, ..p.q, .) from the importing file's package;
 *           absolute (a.b) from the file's folder and each folder above it,
 *           as a module (a/b.py) or a package (a/b/__init__.py)
 *
 * A path that would climb out of the folder names nothing. */
const char *import_resolve(Arena *a, SyntaxLang l, const char *from,
                           const SyntaxImport *imp, ImportExists exists,
                           void *ud);

/* Brings the imports links of one folder source in line with its files'
 * code, after `kb add --dir` has filed them: `files` are the files scanned
 * (text and all, unchanged ones included), `gone` the documents just
 * forgotten because their files are gone. A document whose file is missing
 * and was kept (--no-forget) keeps its links as they were. Counts what was
 * linked and unlinked. */
bool imports_sync(Arena *a, Store *s, const char *source_id,
                  const DirFile *files, size_t nfiles,
                  const char *const *gone, size_t ngone, size_t *linked,
                  size_t *unlinked, char *err, size_t errsz);

#endif /* KB_IMPORTS_H */
