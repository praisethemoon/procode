/* A commit's message: intent and behavior, and the checks `lap commit`
 * runs on them before anything is written (SPEC §Messages).
 *
 * Words are lowercased runs of letters and digits; every byte >= 0x80
 * counts as a letter, so non-ASCII words stay whole. Similarity is the
 * Jaccard index of two word sets.
 */
#ifndef LAP_MSG_H
#define LAP_MSG_H

#include "str.h"

#define MSG_MIN_WORDS 3
#define MSG_MAX_SIMILARITY 0.8

/* Distinct words, sorted. */
typedef struct {
    Str *v;
    int32_t n;
} WordSet;

/* Every word in text, repeats included. */
int32_t msg_word_count(const char *text);
WordSet msg_words(Arena *a, const char *text);
/* The words of a run of lines, as one set. */
WordSet msg_words_lines(Arena *a, const Str *lines, int32_t n);
/* |a ∩ b| / |a ∪ b|; 0 when both are empty. */
double msg_similarity(const WordSet *a, const WordSet *b);

typedef struct {
    const char *intent;
    const char *behavior;
    const char *prev_behavior; /* the session's last behavior; NULL: none */
    const Str *code;           /* the edit's changed lines */
    int32_t code_n;
    bool force; /* --force-message: skips every check but the length */
} MsgInput;

/* NULL when the message passes; otherwise the error code, with a sentence
 * for the author in why. */
const char *msg_check(Arena *a, const MsgInput *in, char *why, size_t whysz);

/* Splits a -F message file into its Intent: and Behavior: sections, each
 * trimmed. Returns false with a reason in err (bad_message_file). */
bool msg_parse_file(Arena *a, const char *text, const char **intent,
                    const char **behavior, char *err, size_t errsz);

/* Trailing spaces, tabs and line ends removed, in place. */
void msg_trim(char *s);

#endif /* LAP_MSG_H */
