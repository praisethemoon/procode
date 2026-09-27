#include "msg.h"

static bool word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c >= 0x80;
}

int32_t msg_word_count(const char *text) {
    int32_t n = 0;
    bool in = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        bool w = word_byte(*p);
        if (w && !in)
            n++;
        in = w;
    }
    return n;
}

static int cmp_str(const void *x, const void *y) {
    const Str *a = (const Str *)x, *b = (const Str *)y;
    size_t n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->ptr, b->ptr, n);
    if (c)
        return c;
    return a->len < b->len ? -1 : a->len > b->len;
}

/* Appends the lowercased words of s[0..len) to *v. */
static void collect(Arena *a, const char *s, size_t len, Str **v,
                    size_t *n, size_t *cap) {
    size_t i = 0;
    while (i < len) {
        while (i < len && !word_byte((unsigned char)s[i]))
            i++;
        size_t start = i;
        while (i < len && word_byte((unsigned char)s[i]))
            i++;
        if (i == start)
            continue;
        char *w = (char *)arena_alloc(a, i - start + 1);
        for (size_t k = start; k < i; k++) {
            char c = s[k];
            w[k - start] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        w[i - start] = '\0';
        ARENA_GROW(a, *v, *n, *cap, Str);
        (*v)[(*n)++] = str_n(w, i - start);
    }
}

static WordSet dedupe(Str *v, size_t n) {
    WordSet ws = {v, 0};
    if (n == 0)
        return ws;
    qsort(v, n, sizeof *v, cmp_str);
    size_t out = 1;
    for (size_t i = 1; i < n; i++) {
        if (!str_eq(v[i], v[out - 1]))
            v[out++] = v[i];
    }
    ws.n = (int32_t)out;
    return ws;
}

WordSet msg_words(Arena *a, const char *text) {
    Str *v = NULL;
    size_t n = 0, cap = 0;
    collect(a, text, strlen(text), &v, &n, &cap);
    return dedupe(v, n);
}

WordSet msg_words_lines(Arena *a, const Str *lines, int32_t count) {
    Str *v = NULL;
    size_t n = 0, cap = 0;
    for (int32_t i = 0; i < count; i++)
        collect(a, lines[i].ptr, lines[i].len, &v, &n, &cap);
    return dedupe(v, n);
}

double msg_similarity(const WordSet *a, const WordSet *b) {
    int32_t i = 0, j = 0, both = 0;
    while (i < a->n && j < b->n) {
        int c = cmp_str(&a->v[i], &b->v[j]);
        if (c == 0) {
            both++;
            i++;
            j++;
        } else if (c < 0) {
            i++;
        } else {
            j++;
        }
    }
    int32_t either = a->n + b->n - both;
    return either ? (double)both / (double)either : 0.0;
}

const char *msg_check(Arena *a, const MsgInput *in, char *why,
                      size_t whysz) {
    if (msg_word_count(in->intent) < MSG_MIN_WORDS) {
        snprintf(why, whysz,
                 "the intent needs at least %d words: say why this edit "
                 "exists",
                 MSG_MIN_WORDS);
        return "message_too_short";
    }
    if (msg_word_count(in->behavior) < MSG_MIN_WORDS) {
        snprintf(why, whysz,
                 "the behavior needs at least %d words: say what this edit "
                 "makes the code do",
                 MSG_MIN_WORDS);
        return "message_too_short";
    }
    if (in->force)
        return NULL;
    WordSet b = msg_words(a, in->behavior);
    WordSet i = msg_words(a, in->intent);
    if (msg_similarity(&b, &i) >= MSG_MAX_SIMILARITY) {
        snprintf(why, whysz,
                 "the behavior repeats the intent; describe what this "
                 "particular edit does");
        return "behavior_repeats_intent";
    }
    if (in->prev_behavior) {
        WordSet p = msg_words(a, in->prev_behavior);
        if (msg_similarity(&b, &p) >= MSG_MAX_SIMILARITY) {
            snprintf(why, whysz,
                     "the behavior repeats the previous commit's; describe "
                     "what this edit does that that one did not");
            return "behavior_repeats_previous";
        }
    }
    WordSet c = msg_words_lines(a, in->code, in->code_n);
    if (msg_similarity(&b, &c) >= MSG_MAX_SIMILARITY) {
        snprintf(why, whysz,
                 "the behavior restates the changed lines; say what they "
                 "make the code do");
        return "behavior_restates_code";
    }
    return NULL;
}

void msg_trim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    s[len] = '\0';
}

/* A header line: exactly "Intent:" or "Behavior:", trailing blanks and a
 * CR allowed. */
static int header_of(const char *line, size_t len) {
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t'))
        len--;
    if (len == 7 && memcmp(line, "Intent:", 7) == 0)
        return 1;
    if (len == 9 && memcmp(line, "Behavior:", 9) == 0)
        return 2;
    return 0;
}

static bool blank(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            return false;
    }
    return true;
}

bool msg_parse_file(Arena *a, const char *text, const char **intent,
                    const char **behavior, char *err, size_t errsz) {
    const char *start[3] = {NULL, NULL, NULL}; /* section text begins */
    const char *end[3] = {NULL, NULL, NULL};
    int32_t cur = 0;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        int h = header_of(p, len);
        if (h) {
            if (start[h]) {
                snprintf(err, errsz, "the %s: section appears twice",
                         h == 1 ? "Intent" : "Behavior");
                return false;
            }
            if (cur)
                end[cur] = p;
            cur = h;
            start[h] = nl ? nl + 1 : p + len;
        } else if (!cur && !blank(p, len)) {
            snprintf(err, errsz,
                     "text before the first Intent: or Behavior: line");
            return false;
        }
        p = nl ? nl + 1 : p + len;
    }
    if (cur)
        end[cur] = p;
    const char **out[3] = {NULL, intent, behavior};
    for (int h = 1; h <= 2; h++) {
        const char *name = h == 1 ? "Intent" : "Behavior";
        if (!start[h]) {
            snprintf(err, errsz, "no %s: section", name);
            return false;
        }
        const char *s = start[h];
        while (s < end[h] && (*s == '\n' || *s == '\r' || *s == ' ' ||
                              *s == '\t'))
            s++;
        char *t = arena_strndup(a, s, (size_t)(end[h] - s));
        msg_trim(t);
        if (!t[0]) {
            snprintf(err, errsz, "the %s: section is empty", name);
            return false;
        }
        *out[h] = t;
    }
    return true;
}
