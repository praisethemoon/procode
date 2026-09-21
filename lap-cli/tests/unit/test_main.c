#include "test.h"

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
void test_diff(void);
void test_ignore(void);
void test_rec(void);

int main(void) {
    test_arena();
    test_str();
    test_sha256();
    test_json();
    test_diff();
    test_ignore();
    test_rec();
    printf("unit tests: %d passed, %d failed\n", t_pass, t_fail);
    return t_fail == 0 ? 0 : 1;
}
