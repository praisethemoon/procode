# kb - local knowledge base over docs, source and papers (C11, zero deps)
#
# Targets:
#   make            build bin/kb
#   make unit       build + run unit tests
#   make e2e        build + run end-to-end tests
#   make test       unit + e2e
#   make install    copy bin/kb to $(PREFIX)/bin (PREFIX=/usr/local)
#   make uninstall  remove it
#   make clean
#
# Windows note: use CMake (CMakeLists.txt) with MSVC or MinGW; this Makefile
# itself targets POSIX systems.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
# `make SIMD=0` builds the portable C loops even where NEON is available, so
# the path every non-ARM machine takes can be tested here.
ifeq ($(SIMD),0)
CFLAGS += -DKB_NO_SIMD
endif
# utf8proc is compiled in, as in the CMake build, which needs this on Windows.
CFLAGS += -DUTF8PROC_STATIC
LDFLAGS ?=
# BM25 needs a natural logarithm. libm is part of the C standard library,
# not a third-party dependency; some systems just keep it in its own file.
LDLIBS := -lm -lpthread
PREFIX ?= /usr/local
DESTDIR ?=

# tree-sitter and its grammars, vendored (src/third_party/TREE-SITTER.md).
# Compiled once into objects under obj/, with warnings off — they are
# someone else's code, and kb's -Wall -Wextra would bury kb's own warnings —
# and linked into every binary below.
TS := src/third_party/tree-sitter
SRC_TS := $(TS)/lib/src/lib.c \
          $(TS)-c/src/parser.c \
          $(TS)-typescript/typescript/src/parser.c $(TS)-typescript/typescript/src/scanner.c \
          $(TS)-typescript/tsx/src/parser.c $(TS)-typescript/tsx/src/scanner.c \
          $(TS)-javascript/src/parser.c $(TS)-javascript/src/scanner.c \
          $(TS)-python/src/parser.c $(TS)-python/src/scanner.c \
          $(TS)-go/src/parser.c \
          $(TS)-rust/src/parser.c $(TS)-rust/src/scanner.c \
          $(TS)-asm/src/parser.c
# Its own variable, so a CFLAGS given on the command line keeps it.
INCLUDES := -I$(TS)/lib/include
OBJ_TS := $(patsubst %.c,obj/%.o,$(SRC_TS))
# tree-sitter's lib.c includes "unicode/umachine.h" and its siblings from
# lib/src, as its own build does. Without this gcc and clang fall back to the
# system's ICU headers: Apple's on macOS, none on a Linux without libicu-dev.
obj/$(TS)/lib/src/lib.o: INCLUDES += -I$(TS)/lib/src

obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -w -c $< -o $@

SRC_LIB := src/arena.c src/str.c src/sha256.c src/json.c src/errdet.c src/platform.c \
           src/doc.c src/store.c src/chunk.c src/token.c src/fts.c \
           src/snippet.c src/gguf.c src/quant.c src/wpm.c src/bpe.c src/gitignore.c src/dirscan.c src/syntax.c src/syntax_tags.c src/embed.c src/modelrec.c src/vectors.c \
           src/third_party/utf8proc/utf8proc.c \
           src/rank.c src/cmd_common.c \
           src/cmd_init.c src/cmd_add.c src/cmd_ls.c src/cmd_get.c \
           src/cmd_search.c src/cmd_chunk.c src/cmd_rebuild.c \
           src/cmd_collections.c src/cmd_status.c \
           src/cmd_stale.c src/cmd_links.c src/cmd_maint.c \
           src/cmd_forget.c src/cmd_sources.c src/help.c
SRC_MAIN := src/main.c
SRC_TESTS := $(wildcard tests/unit/*.c)

BIN := bin/kb
TEST_BIN := bin/kb-unit-tests

all: $(BIN)

$(BIN): $(SRC_LIB) $(SRC_MAIN) src/*.h src/third_party/utf8proc/* $(OBJ_TS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRC_LIB) $(SRC_MAIN) $(OBJ_TS) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(SRC_LIB) $(SRC_TESTS) src/*.h tests/unit/test.h $(OBJ_TS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc -o $@ $(SRC_LIB) $(SRC_TESTS) $(OBJ_TS) $(LDFLAGS) $(LDLIBS)

# A development tool: kb's BPE tokenizer over files, for the check against
# the Hugging Face tokenizer (tools/modernbert/check_tokenizer.py).
bin/kb-bpe-dump: $(SRC_LIB) tests/tools/bpe_dump.c src/*.h $(OBJ_TS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc -o $@ $(SRC_LIB) tests/tools/bpe_dump.c $(OBJ_TS) $(LDFLAGS) $(LDLIBS)

# A development tool: times the embedder, and compares its vectors with a
# saved run, so a faster forward pass can show it computes the same thing.
bin/kb-embed-bench: $(SRC_LIB) tests/tools/embed_bench.c src/*.h $(OBJ_TS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc -o $@ $(SRC_LIB) tests/tools/embed_bench.c $(OBJ_TS) $(LDFLAGS) $(LDLIBS)

# A development tool: kb's grammars over files, to see how well each parses.
bin/kb-syntax-dump: $(SRC_LIB) tests/tools/syntax_dump.c src/*.h $(OBJ_TS)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc -o $@ $(SRC_LIB) tests/tools/syntax_dump.c $(OBJ_TS) $(LDFLAGS) $(LDLIBS)

# The BPE and ModernBERT tests read gte-modernbert-base's model file
# (read-only) and are skipped without it.
KB_TEST_MODERNBERT ?= $(firstword $(wildcard $(HOME)/.kb/models/gte-modernbert-base*.gguf))
KB_TEST_RERANKER ?= $(firstword $(wildcard $(HOME)/.kb/models/gte-reranker-modernbert-base*.gguf))

unit: $(TEST_BIN)
	KB_TEST_MODERNBERT="$(KB_TEST_MODERNBERT)" KB_TEST_RERANKER="$(KB_TEST_RERANKER)" ./$(TEST_BIN)

# The model tests run against the real weights when this machine has them
# (the one .gguf in ~/.kb/models, linked into the run's throwaway HOME) and
# are skipped otherwise. KB_TEST_MODEL=<file> names another.
KB_TEST_MODEL ?= $(firstword $(wildcard $(HOME)/.kb/models/nomic-embed-text*.gguf))

e2e: $(BIN) $(TEST_BIN)
	KB="$(abspath $(BIN))" KB_TESTS="$(abspath $(TEST_BIN))" \
	  KB_TEST_MODEL="$(KB_TEST_MODEL)" KB_TEST_MODERNBERT="$(KB_TEST_MODERNBERT)" \
	  sh tests/e2e/run.sh

test: unit e2e

install: $(BIN)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/kb"
	@echo "installed $(DESTDIR)$(PREFIX)/bin/kb"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/kb"
	@echo "removed $(DESTDIR)$(PREFIX)/bin/kb"

clean:
	rm -rf bin obj

.PHONY: all unit e2e test install uninstall clean
