/* §5 — provenance and staleness: the duration grammar, the timestamp
 * arithmetic underneath it, and the one comparison every route shares.
 *
 * These are the pieces that decide whether a passage is believed. A
 * comparison that ran the wrong way, or a unit that was ignored, would give
 * every route a confident wrong answer rather than a visible failure — so
 * each is pinned against a value worked out independently rather than
 * against whatever the code happens to produce.
 */
#include "cmd.h"
#include "test.h"

/* ---- durations --------------------------------------------------------- */

static void test_durations(void) {
    int64_t s = -1;

    t_begin("stale: every unit means what it says");
    ASSERT_TRUE(duration_parse("1s", &s));
    ASSERT_EQ_I(s, 1);
    ASSERT_TRUE(duration_parse("1m", &s));
    ASSERT_EQ_I(s, 60);
    ASSERT_TRUE(duration_parse("1h", &s));
    ASSERT_EQ_I(s, 3600);
    ASSERT_TRUE(duration_parse("1d", &s));
    ASSERT_EQ_I(s, 86400);
    ASSERT_TRUE(duration_parse("1w", &s));
    ASSERT_EQ_I(s, 604800);

    t_begin("stale: the count multiplies the unit");
    /* §5's own example. A parser that read the number and dropped the unit
     * would answer 90 here and would call a three-month-old page fresh. */
    ASSERT_TRUE(duration_parse("90d", &s));
    ASSERT_EQ_I(s, 7776000);
    ASSERT_TRUE(duration_parse("2w", &s));
    ASSERT_EQ_I(s, 1209600);
    ASSERT_TRUE(duration_parse("36h", &s));
    ASSERT_EQ_I(s, 129600);

    t_begin("stale: zero is a threshold, not an absence");
    /* "older than nothing" is every document, which is a meaningful thing to
     * ask for and the only way to see the whole corpus through this route. */
    ASSERT_TRUE(duration_parse("0d", &s));
    ASSERT_EQ_I(s, 0);

    t_begin("stale: a bare number is refused rather than guessed at");
    ASSERT_TRUE(!duration_parse("90", &s));
    ASSERT_TRUE(!duration_parse("0", &s));

    t_begin("stale: a unit this build does not have is refused");
    /* A month and a year are calendar quantities whose length depends on
     * when you start counting. Accepting "1y" as 365 days would be a
     * different answer from the one the caller means in a leap year. */
    ASSERT_TRUE(!duration_parse("1y", &s));
    ASSERT_TRUE(!duration_parse("1M", &s));
    ASSERT_TRUE(!duration_parse("1D", &s));

    t_begin("stale: nothing else is a duration");
    ASSERT_TRUE(!duration_parse(NULL, &s));
    ASSERT_TRUE(!duration_parse("", &s));
    ASSERT_TRUE(!duration_parse("d", &s));
    ASSERT_TRUE(!duration_parse("-1d", &s));
    ASSERT_TRUE(!duration_parse("90dd", &s));
    ASSERT_TRUE(!duration_parse("90 d", &s));
    ASSERT_TRUE(!duration_parse("9.5d", &s));
    ASSERT_TRUE(!duration_parse("90days", &s));

    t_begin("stale: a count that cannot fit is refused, not wrapped");
    ASSERT_TRUE(!duration_parse("99999999999999999999d", &s));
    ASSERT_TRUE(!duration_parse("9223372036854775807w", &s));
}

/* ---- the timestamp, both directions ------------------------------------ */

static void test_timestamps(void) {
    int64_t at = -1;

    t_begin("stale: a timestamp parses to the second it names");
    /* Values taken from an independent calendar, not from this code. */
    ASSERT_TRUE(plat_time_parse("1970-01-01T00:00:00Z", &at));
    ASSERT_EQ_I(at, 0);
    ASSERT_TRUE(plat_time_parse("1970-01-02T00:00:00Z", &at));
    ASSERT_EQ_I(at, 86400);
    ASSERT_TRUE(plat_time_parse("2001-09-09T01:46:40Z", &at));
    ASSERT_EQ_I(at, 1000000000);
    ASSERT_TRUE(plat_time_parse("2026-09-20T12:34:56Z", &at));
    ASSERT_EQ_I(at, 1789907696);
    ASSERT_TRUE(plat_time_parse("1999-12-31T23:59:59Z", &at));
    ASSERT_EQ_I(at, 946684799);

    t_begin("stale: leap years are the ones the calendar actually has");
    /* 2000 is a leap year and 2100 is not — the century rule, which a
     * "divisible by four" shortcut gets wrong by a day in both directions. */
    ASSERT_TRUE(plat_time_parse("2024-02-29T12:00:00Z", &at));
    ASSERT_EQ_I(at, 1709208000);
    ASSERT_TRUE(plat_time_parse("2000-02-29T00:00:00Z", &at));
    ASSERT_EQ_I(at, 951782400);
    ASSERT_TRUE(plat_time_parse("2100-02-28T23:59:59Z", &at));
    ASSERT_EQ_I(at, 4107542399);
    ASSERT_TRUE(!plat_time_parse("2100-02-29T00:00:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2023-02-29T00:00:00Z", &at));

    t_begin("stale: a date that is not a date is refused, not reinterpreted");
    /* Every value here comes out of a log line somebody can edit. A lenient
     * parser would silently read "2026-02-31" as the first of March. */
    ASSERT_TRUE(!plat_time_parse("2026-02-31T00:00:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-13-01T00:00:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-00-01T00:00:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-01-00T00:00:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-01-01T24:00:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-01-01T00:60:00Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-01-01T00:00:60Z", &at));

    t_begin("stale: only the layout this build writes is accepted");
    ASSERT_TRUE(!plat_time_parse(NULL, &at));
    ASSERT_TRUE(!plat_time_parse("", &at));
    ASSERT_TRUE(!plat_time_parse("2026-09-20", &at));
    ASSERT_TRUE(!plat_time_parse("2026-09-20T12:34:56", &at));
    ASSERT_TRUE(!plat_time_parse("2026-09-20T12:34:56+00:00", &at));
    ASSERT_TRUE(!plat_time_parse("2026-09-20 12:34:56Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026/09/20T12:34:56Z", &at));
    ASSERT_TRUE(!plat_time_parse("2026-9-20T12:34:56Z", &at));
    ASSERT_TRUE(!plat_time_parse(" 026-09-20T12:34:56Z", &at));
    ASSERT_TRUE(!plat_time_parse("+026-09-20T12:34:56Z", &at));

    t_begin("stale: the formatter and the parser are inverses");
    /* plat_timestamp is plat_time_format of now, so a value this parses is a
     * value that formatter wrote. If the two disagreed, every document ever
     * filed would be unreadable to §5. */
    static const int64_t points[] = {0,          86400,      1000000000,
                                     1789907696, 951782400,  4107542399,
                                     1798761599};
    for (size_t i = 0; i < sizeof points / sizeof *points; i++) {
        char iso[32];
        plat_time_format(points[i], iso);
        int64_t back = -1;
        ASSERT_TRUE(plat_time_parse(iso, &back));
        ASSERT_EQ_I(back, points[i]);
    }

    t_begin("stale: the clock reads as a timestamp this build can parse");
    char now[32];
    plat_timestamp(now);
    int64_t parsed = -1;
    ASSERT_TRUE(plat_time_parse(now, &parsed));
    /* Sampled a moment apart, so they are the same second or adjacent ones. */
    int64_t epoch = plat_now_epoch();
    ASSERT_TRUE(epoch - parsed >= 0 && epoch - parsed <= 2);
}

/* ---- the one comparison ------------------------------------------------ */

static Document doc_at(const char *fetched_at) {
    Document d;
    memset(&d, 0, sizeof d);
    d.id = "D-1";
    d.source = "S-1";
    d.path = "";
    d.content_hash = "cafe";
    d.fetched_at = fetched_at;
    return d;
}

/* A threshold anchored to a fixed instant rather than to the clock, so the
 * assertions below are about the comparison and not about when they ran. */
static Staleness at(const char *now_iso, int64_t seconds) {
    Staleness st;
    memset(&st, 0, sizeof st);
    st.spec = "fixed";
    st.seconds = seconds;
    plat_time_parse(now_iso, &st.now);
    st.cutoff = st.now - seconds;
    plat_time_format(st.cutoff, st.cutoff_iso);
    return st;
}

static void test_verdict(void) {
    /* Ninety days before 2026-09-25T00:00:00Z. */
    Staleness st = at("2026-09-25T00:00:00Z", 90 * 86400);

    t_begin("stale: the comparison runs the way round §5 says");
    ASSERT_EQ_S(st.cutoff_iso, "2026-06-27T00:00:00Z");
    /* OLDER than the threshold is stale. A comparison flipped the other way
     * would call yesterday's page stale and a three-year-old page fresh —
     * and would still pass any test that only looked at one document. */
    Document ancient = doc_at("2020-01-01T00:00:00Z");
    Document fresh = doc_at("2026-09-24T23:00:00Z");
    ASSERT_TRUE(doc_stale(&st, &ancient));
    ASSERT_TRUE(!doc_stale(&st, &fresh));

    t_begin("stale: the boundary belongs to the window the caller asked for");
    Document exactly = doc_at("2026-06-27T00:00:00Z");
    Document one_second_older = doc_at("2026-06-26T23:59:59Z");
    Document one_second_newer = doc_at("2026-06-27T00:00:01Z");
    ASSERT_TRUE(!doc_stale(&st, &exactly));
    ASSERT_TRUE(doc_stale(&st, &one_second_older));
    ASSERT_TRUE(!doc_stale(&st, &one_second_newer));

    t_begin("stale: the threshold is what moves the verdict");
    /* The same document, two thresholds. If the unit or the count were
     * ignored these would agree. */
    Document may = doc_at("2026-05-01T00:00:00Z");
    Staleness tight = at("2026-09-25T00:00:00Z", 30 * 86400);
    Staleness loose = at("2026-09-25T00:00:00Z", 365 * 86400);
    ASSERT_TRUE(doc_stale(&tight, &may));
    ASSERT_TRUE(!doc_stale(&loose, &may));

    t_begin("stale: zero makes everything stale and nothing crash");
    Staleness none = at("2026-09-25T00:00:00Z", 0);
    ASSERT_TRUE(doc_stale(&none, &ancient));
    ASSERT_TRUE(doc_stale(&none, &fresh));
    Document later = doc_at("2026-09-25T00:00:00Z");
    ASSERT_TRUE(!doc_stale(&none, &later));

    t_begin("stale: a document that cannot say how old it is, is stale");
    /* §5 exists because "a passage that cannot say how old it is will
     * eventually be believed when it should not be". One with no readable
     * date cannot say at all, so it gets the answer that makes a reader
     * look rather than the one that makes them trust. */
    Document no_date = doc_at(NULL);
    Document empty = doc_at("");
    Document garbage = doc_at("yesterday");
    Document impossible = doc_at("2026-02-31T00:00:00Z");
    ASSERT_TRUE(doc_stale(&st, &no_date));
    ASSERT_TRUE(doc_stale(&st, &empty));
    ASSERT_TRUE(doc_stale(&st, &garbage));
    ASSERT_TRUE(doc_stale(&st, &impossible));

    t_begin("stale: staleness_init defaults to the threshold §5 uses");
    char err[512];
    Staleness d;
    ASSERT_TRUE(staleness_init(&d, NULL, err, sizeof err));
    ASSERT_EQ_S(d.spec, KB_STALE_DEFAULT);
    ASSERT_EQ_I(d.seconds, 7776000);
    ASSERT_EQ_I(d.now - d.cutoff, 7776000);
    int64_t back = -1;
    ASSERT_TRUE(plat_time_parse(d.cutoff_iso, &back));
    ASSERT_EQ_I(back, d.cutoff);

    t_begin("staleness_init: an override is taken and a bad one refused");
    ASSERT_TRUE(staleness_init(&d, "2w", err, sizeof err));
    ASSERT_EQ_I(d.seconds, 1209600);
    ASSERT_EQ_S(d.spec, "2w");
    /* An empty value is the flag not given, not a threshold of nothing. */
    ASSERT_TRUE(staleness_init(&d, "", err, sizeof err));
    ASSERT_EQ_S(d.spec, KB_STALE_DEFAULT);
    ASSERT_TRUE(!staleness_init(&d, "90", err, sizeof err));
    ASSERT_TRUE(strstr(err, "s m h d w") != NULL);
}

void test_stale(void) {
    test_durations();
    test_timestamps();
    test_verdict();
}
