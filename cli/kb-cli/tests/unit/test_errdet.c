#include "errdet.h"
#include "json.h"
#include "test.h"

void test_errdet(void) {
    t_begin("errdet: fields are an object under their own code");
    errdet_begin("store_locked");
    errdet_str("store", "/w/.kb");
    errdet_int("pid", 4242);
    ASSERT_EQ_S(errdet_json("store_locked"),
                "{\"store\":\"/w/.kb\",\"pid\":4242}");
    ASSERT_TRUE(errdet_json("not_found") == NULL);

    t_begin("errdet: a new code discards the old fields");
    errdet_begin("index_stale");
    ASSERT_TRUE(errdet_json("index_stale") == NULL);
    const char *const s[] = {"keyword", "vectors"};
    errdet_strs("structures", s, 2);
    ASSERT_EQ_S(errdet_json("index_stale"),
                "{\"structures\":[\"keyword\",\"vectors\"]}");

    t_begin("errdet: strings are escaped");
    errdet_begin("fetch_failed");
    errdet_str("locator", "/a \"b\"\\c\n");
    ASSERT_EQ_S(errdet_json("fetch_failed"),
                "{\"locator\":\"/a \\\"b\\\"\\\\c\\u000a\"}");

    t_begin("errdet: a field that does not fit is dropped whole");
    errdet_begin("unsupported_mime");
    errdet_str("mime", "application/pdf");
    char big[4096];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    errdet_str("huge", big);
    ASSERT_EQ_S(errdet_json("unsupported_mime"),
                "{\"mime\":\"application/pdf\"}");
    Arena *a = arena_new(1 << 12);
    char err[128];
    ASSERT_TRUE(json_parse(a, errdet_json("unsupported_mime"),
                           strlen(errdet_json("unsupported_mime")), err,
                           sizeof err) != NULL);
    arena_free(a);
}
