/* The branch registry: .lap/branches.json in a parent folder lists the
 * branches started from it and where their folders are. It is
 * machine-local (a path means nothing on another machine), neither history
 * nor a cache — nothing rebuilds it — and hints only: no command fails
 * because of what it says, and an entry is checked before it is used.
 */
#ifndef LAP_BRANCHES_H
#define LAP_BRANCHES_H

#include "rec.h"

#define LAP_BRANCHES_NAME "branches.json"

typedef struct {
    const char *id;      /* the branch's lineage, 12 hex digits */
    const char *name;    /* what people type */
    const char *path;    /* the branch folder, absolute, '/' separators */
    const char *base;    /* the parent's head the branch started from */
    const char *started; /* ISO-8601 UTC */
    const char *via;     /* not saved: found in this branch's registry by
                            branches_load_deep; NULL for this folder's own */
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
/* branches_load, then the registry of every listed branch whose folder is
 * still that branch, and theirs in turn: the branches started from this
 * folder or from any of its branches, each nested one with via set. Read
 * only: saving it would copy other folders' entries here. */
void branches_load_deep(Arena *a, const char *lapdir, Branches *out);
/* Writes lapdir/branches.json atomically. */
bool branches_save(Arena *a, const char *lapdir, const Branches *b);
/* The entry whose id or name is key, or NULL. */
const BranchEntry *branches_find(const Branches *b, const char *key);
void branches_add(Arena *a, Branches *b, BranchEntry e);
/* The first entry of b that is a live branch of folder: its folder is
 * still that branch and its .lap/parent names folder (however spelled).
 * NULL when there is none — b is then a copy of another folder's registry
 * (brought by copying the folder), not folder's own. */
const BranchEntry *branches_live_of(Arena *a, const Branches *b,
                                    const char *folder);
/* Whether folder root (its .lap at lapdir), whose .lap/lineage names
 * branch `lineage`, is a copy of that branch rather than the branch: its
 * .lap/parent's registry places the branch in another folder, which is
 * there and still that branch. *original is then that folder. A branch
 * moved away (its registered folder gone, or no longer that branch) is not
 * a copy: it is the branch, not yet told where it went. */
bool branches_copy_of(Arena *a, const char *lapdir, const char *root,
                      const char *lineage, const char **original);

/* What the parent knows of one registered branch. An entry is checked,
 * never trusted: its folder counts only while it exists and its .lap is
 * that branch. The branch's history is read from that folder, else from
 * its chunks in the parent. */
typedef struct {
    bool present;       /* its folder is there and still this branch */
    bool readable;      /* its history could be read */
    const char *head;   /* the hash of its last record, when readable */
    const char *merged; /* the head the last merge of it adopted, or NULL */
    int32_t since_base; /* commits after its start; -1 when not readable */
    int32_t since_merge; /* commits after the last merged head; -1 idem */
    const char **stopped; /* files a merge of it stopped, for good */
    const char **stopped_at; /* the first commit to each not adopted */
    int32_t nstopped;
    /* "active", "merged" (adopted up to its head), "partly merged" (a file
     * stopped), or "missing" (its folder is gone, and it was not merged
     * up to its head) */
    const char *state;
} BranchStatus;

/* The status of entry e of the registry in lapdir, whose own history
 * (holding any merge records) is log. */
void branches_status(Arena *a, const char *lapdir, const RecLog *log,
                     const BranchEntry *e, BranchStatus *out);

#endif /* LAP_BRANCHES_H */
