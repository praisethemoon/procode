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
LDFLAGS ?=
# BM25 needs a natural logarithm. libm is part of the C standard library,
# not a third-party dependency; some systems just keep it in its own file.
LDLIBS := -lm -lpthread
PREFIX ?= /usr/local
DESTDIR ?=

SRC_LIB := src/arena.c src/str.c src/sha256.c src/json.c src/errdet.c src/platform.c \
           src/doc.c src/store.c src/chunk.c src/token.c src/fts.c \
           src/snippet.c src/gguf.c src/quant.c src/wpm.c src/bpe.c src/embed.c src/modelrec.c src/vectors.c \
           src/third_party/utf8proc/utf8proc.c \
           src/rank.c src/cmd_common.c \
           src/cmd_init.c src/cmd_add.c src/cmd_ls.c src/cmd_get.c \
           src/cmd_search.c src/cmd_chunk.c src/cmd_rebuild.c \
           src/cmd_collections.c src/cmd_status.c \
           src/cmd_stale.c src/cmd_links.c src/cmd_maint.c \
           src/cmd_forget.c src/cmd_sources.c
SRC_MAIN := src/main.c
SRC_TESTS := $(wildcard tests/unit/*.c)

BIN := bin/kb
TEST_BIN := bin/kb-unit-tests

all: $(BIN)

$(BIN): $(SRC_LIB) $(SRC_MAIN) src/*.h src/third_party/utf8proc/*
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC_LIB) $(SRC_MAIN) $(LDFLAGS) $(LDLIBS)

$(TEST_BIN): $(SRC_LIB) $(SRC_TESTS) src/*.h tests/unit/test.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC_LIB) $(SRC_TESTS) $(LDFLAGS) $(LDLIBS)

# A development tool: kb's BPE tokenizer over files, for the check against
# the Hugging Face tokenizer (tools/modernbert/check_tokenizer.py).
bin/kb-bpe-dump: $(SRC_LIB) tests/tools/bpe_dump.c src/*.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC_LIB) tests/tools/bpe_dump.c $(LDFLAGS) $(LDLIBS)

# The BPE and ModernBERT tests read gte-modernbert-base's model file
# (read-only) and are skipped without it.
KB_TEST_MODERNBERT ?= $(firstword $(wildcard $(HOME)/.kb/models/gte-modernbert-base*.gguf))

unit: $(TEST_BIN)
	KB_TEST_MODERNBERT="$(KB_TEST_MODERNBERT)" ./$(TEST_BIN)

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
	rm -rf bin

.PHONY: all unit e2e test install uninstall clean
