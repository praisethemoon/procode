#include "platform.h"
#include "test.h"

#include <time.h>

#ifndef _WIN32
static void zone(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
}

static const char *local(const char *utc, bool offset) {
    static char out[40];
    plat_ts_local(utc, offset, out);
    return out;
}

static const char *parsed(const char *in) {
    static char out[32];
    return plat_ts_parse(in, out) ? out : "(refused)";
}
#endif

void test_time(void) {
#ifndef _WIN32
    const char *saved = getenv("TZ");
    char keep[128] = "";
    if (saved)
        snprintf(keep, sizeof keep, "%s", saved);

    t_begin("time: a log timestamp shown in the machine's zone");
    zone("UTC");
    ASSERT_EQ_S(local("2026-09-27T09:36:48Z", false), "2026-09-27 09:36:48");
    ASSERT_EQ_S(local("2026-09-27T09:36:48Z", true),
                "2026-09-27 09:36:48 +00:00");
    zone("Asia/Kolkata");
    ASSERT_EQ_S(local("2026-09-27T09:36:48Z", true),
                "2026-09-27 15:06:48 +05:30");
    zone("America/New_York");
    ASSERT_EQ_S(local("2026-09-27T09:36:48Z", true),
                "2026-09-27 05:36:48 -04:00");
    ASSERT_EQ_S(local("2026-12-27T09:36:48Z", true),
                "2026-12-27 04:36:48 -05:00");
    ASSERT_EQ_S(local("not a time", true), "not a time");

    t_begin("time: a person's time becomes a log timestamp");
    zone("Europe/Berlin");
    ASSERT_EQ_S(parsed("2026-09-27 11:36:48"), "2026-09-27T09:36:48Z");
    ASSERT_EQ_S(parsed("2026-09-27T11:36"), "2026-09-27T09:36:00Z");
    ASSERT_EQ_S(parsed("2026-09-27"), "2026-09-26T22:00:00Z");
    ASSERT_EQ_S(parsed("2026-09-27T09:36:48Z"), "2026-09-27T09:36:48Z");
    ASSERT_EQ_S(parsed("2026-09-27 15:06:48+05:30"), "2026-09-27T09:36:48Z");
    ASSERT_EQ_S(parsed("2026-09-27 05:36:48-0400"), "2026-09-27T09:36:48Z");
    /* 02:30 happens twice on 2026-10-25 in Berlin: the earlier (CEST) */
    ASSERT_EQ_S(parsed("2026-10-25 02:30"), "2026-10-25T00:30:00Z");
    ASSERT_EQ_S(parsed("yesterday"), "(refused)");
    ASSERT_EQ_S(parsed("2026-13-01"), "(refused)");
    ASSERT_EQ_S(parsed("2026-09-27 25:00"), "(refused)");
    ASSERT_EQ_S(parsed("2026-09-27 11:36 later"), "(refused)");

    if (saved)
        zone(keep);
    else {
        unsetenv("TZ");
        tzset();
    }
#endif
}
