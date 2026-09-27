#include "msg.h"
#include "test.h"

static const char *check(Arena *a, const char *intent, const char *behavior,
                         const char *prev, const char *code, bool force) {
    Str line = str_c(code ? code : "");
    MsgInput in = {intent, behavior, prev, &line, code ? 1 : 0, force};
    char why[256];
    return msg_check(a, &in, why, sizeof why);
}

void test_msg(void) {
    Arena *a = arena_new(0);

    t_begin("msg: words split on non-alphanumerics, lowercased, deduped");
    ASSERT_EQ_I(msg_word_count("Route the graph-place to its view."), 7);
    ASSERT_EQ_I(msg_word_count("  --- "), 0);
    ASSERT_EQ_I(msg_word_count("café crème"), 2);
    WordSet w = msg_words(a, "The the THE graph");
    ASSERT_EQ_I(w.n, 2);

    t_begin("msg: similarity is the Jaccard index of the word sets");
    WordSet x = msg_words(a, "a b c d");
    WordSet y = msg_words(a, "c d e f");
    ASSERT_TRUE(msg_similarity(&x, &y) > 0.33 && msg_similarity(&x, &y) < 0.34);
    WordSet none = msg_words(a, "");
    ASSERT_TRUE(msg_similarity(&none, &none) == 0.0);

    t_begin("msg: a good message passes");
    ASSERT_TRUE(check(a, "Route the graph place to its view.",
                      "Render Graph when the target place is graph.",
                      "Import Graph for the graph route.", "return <Graph/>;",
                      false) == NULL);

    t_begin("msg: each check refuses with its own code");
    ASSERT_EQ_S(check(a, "fix it", "Render Graph for graph places.", NULL,
                      NULL, false),
                "message_too_short");
    ASSERT_EQ_S(check(a, "Route the graph place.", "graph", NULL, NULL, false),
                "message_too_short");
    ASSERT_EQ_S(check(a, "Route the graph place to its view.",
                      "Route the graph place to its view", NULL, NULL, false),
                "behavior_repeats_intent");
    ASSERT_EQ_S(check(a, "Route the graph place to its view.",
                      "Render Graph for the graph place.",
                      "render graph for the graph place", NULL, false),
                "behavior_repeats_previous");
    ASSERT_EQ_S(check(a, "Route the graph place to its view.",
                      "return Graph if place is graph",
                      NULL, "if (place === 'graph') return <Graph/>;", false),
                "behavior_restates_code");

    t_begin("msg: --force-message skips every check but the length");
    ASSERT_TRUE(check(a, "Route the graph place to its view.",
                      "Route the graph place to its view", NULL, NULL,
                      true) == NULL);
    ASSERT_EQ_S(check(a, "Route the graph place.", "graph", NULL, NULL, true),
                "message_too_short");

    t_begin("msg: -F file sections, in either order, trimmed");
    const char *intent, *behavior;
    char err[256];
    ASSERT_TRUE(msg_parse_file(a,
                               "\nIntent:\nRoute the graph.\n  second line\n\n"
                               "Behavior:\r\nRender Graph.\n\n",
                               &intent, &behavior, err, sizeof err));
    ASSERT_EQ_S(intent, "Route the graph.\n  second line");
    ASSERT_EQ_S(behavior, "Render Graph.");
    ASSERT_TRUE(msg_parse_file(a, "Behavior:\nb b b\nIntent:\ni i i",
                               &intent, &behavior, err, sizeof err));
    ASSERT_EQ_S(intent, "i i i");
    ASSERT_EQ_S(behavior, "b b b");

    t_begin("msg: -F files that are refused");
    ASSERT_TRUE(!msg_parse_file(a, "preamble\nIntent:\ni\nBehavior:\nb",
                                &intent, &behavior, err, sizeof err));
    ASSERT_TRUE(!msg_parse_file(a, "Intent:\ni\n", &intent, &behavior, err,
                                sizeof err));
    ASSERT_TRUE(!msg_parse_file(a, "Intent:\ni\nIntent:\nj\nBehavior:\nb",
                                &intent, &behavior, err, sizeof err));
    ASSERT_TRUE(!msg_parse_file(a, "Intent:\n  \nBehavior:\nb", &intent,
                                &behavior, err, sizeof err));
    ASSERT_TRUE(!msg_parse_file(a, "intent: lower case is not a header\n",
                                &intent, &behavior, err, sizeof err));

    arena_free(a);
}
