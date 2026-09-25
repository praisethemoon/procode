/* kb - local offline knowledge base over documentation and source trees.
 * Common definitions shared by every translation unit.
 */
#ifndef KB_H
#define KB_H

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KB_VERSION "0.1.0"

#define KB_DIR ".kb"
#define KB_SOURCES_NAME "sources.jsonl"
#define KB_DOCUMENTS_NAME "documents.jsonl"
#define KB_BLOBS_NAME "blobs"
#define KB_INDEX_NAME "index"
#define KB_GITIGNORE_NAME ".gitignore"
/* Both live under index/ rather than beside the logs: §1.5 commits exactly
 * sources.jsonl, documents.jsonl and blobs/, and the .gitignore we write
 * ignores index/ and nothing else. Anything machine-local therefore has to
 * be under index/ or it lands in the repository. The lock names a live pid
 * on this machine and the counters are a cache of the logs' highest id, so
 * both are machine-local by construction. */
#define KB_LOCK_NAME "index/lock"
#define KB_COUNTERS_NAME "index/counters.json"
#define KB_MODEL_NAME "index/model.json"
#define KB_FTS_NAME "index/fts.db"

#define KB_PATH_MAX 4096
/* Refuse content larger than this. The logs themselves are read uncapped
 * (they must never become unreadable by growing); this bounds only content
 * handed in by a caller. */
#define KB_MAX_FILE_SIZE (64u * 1024u * 1024u)

/* Chunking parameters, written to index/model.json at first ingest (§3, §8).
 * They belong to the model's context window, so the embedder will own them
 * once it exists; until then these are the defaults the chunker applies.
 *
 * KB_CHUNKER_ID versions the *algorithm*. §8 records only the sizes, but two
 * indexes built with identical sizes and different splitting rules are just
 * as incompatible, and a later reindex has no other way to notice. */
#define KB_CHUNKER_ID "structural-1"
#define KB_CHUNK_TOKENS 400u
#define KB_CHUNK_OVERLAP 60u
/* No tokenizer exists until the model loads, so a token is approximated as
 * four bytes — the usual ratio for English prose and close enough for code.
 * Every conversion goes through this one constant so the real tokenizer has
 * a single place to replace. */
#define KB_BYTES_PER_TOKEN 4u

/* ---- keyword retrieval (§4) --------------------------------------------
 *
 * Two version stamps, both written into index/fts.db's header and both part
 * of its staleness digest, because either one changing makes the file on
 * disk mean something other than what this build would read out of it.
 *
 * KB_FTS_VERSION versions the FILE: its sections, its record sizes, its
 * byte order. KB_TOKENIZER_VERSION versions the TERMS: what counts as a
 * word, how case is folded, which compounds are split. A file can be
 * perfectly well-formed and still be unusable because it was built by a
 * tokeniser that kept `_` when this one splits on it, and nothing about the
 * bytes would reveal that. Bump the tokeniser stamp for any change to
 * token.c that could produce a different term for the same text.
 */
#define KB_FTS_VERSION 1u
#define KB_TOKENIZER_VERSION 1u

/* BM25's two parameters, in thousandths so the header stays integral. The
 * defaults everyone uses: k1 saturates term frequency, b discounts long
 * chunks. They are stored in the index, so a build that changes them is a
 * change the file reports rather than one that silently reranks a corpus. */
#define KB_BM25_K1_MILLI 1200u
#define KB_BM25_B_MILLI 750u

/* Reciprocal rank fusion's constant (rank.h). */
#define KB_RRF_K 60u

/* §4: k defaults to 10 and is capped at 100. The cap is the same rule as
 * "search returns snippets only" — a caller must not be able to ask for a
 * list big enough to bury whatever it was reading. */
#define KB_SEARCH_K_DEFAULT 10
#define KB_SEARCH_K_MAX 100
/* One snippet's byte budget, and how many neighbouring chunks `expand` may
 * ask for on each side. 100 hits × (1 + 2×3) snippets × 240 bytes is about
 * 170 KB at the absolute maximum a caller can request; the default request
 * is 10 × 240, about 2 KB. `kb chunk` is the route to full text. */
#define KB_SNIPPET_BYTES 240u
#define KB_EXPAND_MAX 3

/* Exit codes: 0 success, 1 user/store error, 2 internal error. */
#define KB_EXIT_OK 0
#define KB_EXIT_ERR 1
#define KB_EXIT_FATAL 2

#endif /* KB_H */
