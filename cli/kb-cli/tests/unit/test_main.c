#include "test.h"

#include "store.h"

int32_t t_pass = 0;
int32_t t_fail = 0;
const char *t_current = "";

void t_begin(const char *name) {
    t_current = name;
}

void test_arena(void);
void test_str(void);
void test_sha256(void);
void test_json(void);
void test_doc(void);
void test_store(void);
void test_chunk(void);
void test_token(void);
void test_fts(void);
void test_rank(void);
void test_snippet(void);
void test_stale(void);
void test_link(void);
void test_errdet(void);
void test_embed(void);
void test_bpe(void);

/* Holds the write lock on a store until stdin closes, so the end-to-end
 * script can see what a *second* process meets: cross-process locking is
 * exactly the property a single-process test can be wrong about. Waiting on
 * EOF rather than on a timer keeps the script free of races. */
static int32_t hold_lock(const char *dir) {
    Arena *a = arena_new(1 << 14);
    Store s;
    char err[512];
    const char *code;
    if (!store_open(a, &s, dir, true, err, sizeof err, &code)) {
        fprintf(stderr, "hold-lock: %s (%s)\n", err, code);
        return 1;
    }
    printf("locked %lld\n", (long long)plat_pid());
    fflush(stdout);
    while (getchar() != EOF) {
        /* drain */
    }
    store_close(&s);
    arena_free(a);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--hold-lock") == 0)
        return (int)hold_lock(argv[2]);

    test_arena();
    test_str();
    test_sha256();
    test_json();
    test_doc();
    test_store();
    test_chunk();
    test_token();
    test_fts();
    test_rank();
    test_snippet();
    test_stale();
    test_link();
    test_errdet();
    test_embed();
    test_bpe();
    printf("unit tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail == 0 ? 0 : 1;
}
