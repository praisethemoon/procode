/* The branch registry: .lap/branches.json in a parent folder lists the
 * branches started from it and where their folders are. It is
 * machine-local (a path means nothing on another machine), neither history
 * nor a cache — nothing rebuilds it — and hints only: no command fails
 * because of what it says, and an entry is checked before it is used.
 */
#ifndef LAP_BRANCHES_H
#define LAP_BRANCHES_H

#include "arena.h"

#define LAP_BRANCHES_NAME "branches.json"

typedef struct {
    const char *id;      /* the branch's lineage, 12 hex digits */
    const char *name;    /* what people type */
    const char *path;    /* the branch folder, absolute, '/' separators */
    const char *base;    /* the parent's head the branch started from */
    const char *started; /* ISO-8601 UTC */
} BranchEntry;

typedef struct {
    BranchEntry *v;
    int32_t n;
    int32_t cap;
} Branches;

/* Reads lapdir/branches.json. A missing or unreadable file, or one that is
 * not the registry's shape, reads as no branches: hints never fail a
 * command. Entries missing a field are skipped. */
void branches_load(Arena *a, const char *lapdir, Branches *out);
/* Writes lapdir/branches.json atomically. */
bool branches_save(Arena *a, const char *lapdir, const Branches *b);
/* The entry whose id or name is key, or NULL. */
const BranchEntry *branches_find(const Branches *b, const char *key);
void branches_add(Arena *a, Branches *b, BranchEntry e);

#endif /* LAP_BRANCHES_H */
