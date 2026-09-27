# lap - fine-grained edit recorder for AI agents (C11, zero dependencies)
#
# Targets:
#   make            build bin/lap
#   make unit       build + run unit tests
#   make e2e        build + run end-to-end tests
#   make test       unit + e2e
#   make install    copy bin/lap to $(PREFIX)/bin (PREFIX=/usr/local)
#   make uninstall  remove it
#   make clean
#
# Windows note: use CMake (CMakeLists.txt) with MSVC or MinGW; this Makefile
# itself targets POSIX systems.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -g
LDFLAGS ?=
PREFIX ?= /usr/local
DESTDIR ?=

SRC_LIB := src/arena.c src/str.c src/sha256.c src/json.c src/platform.c \
           src/ignore.c src/diff.c src/rec.c src/msg.c src/repo.c src/cmd_common.c \
           src/cmd_init.c src/cmd_status.c src/cmd_commit.c src/cmd_log.c \
           src/cmd_show.c src/cmd_search.c src/cmd_session.c src/cmd_verify.c \
           src/cmd_rebuild.c src/cmd_rr.c src/idx.c src/snap.c \
           src/tty.c
SRC_MAIN := src/main.c
SRC_TESTS := $(wildcard tests/unit/*.c)

BIN := bin/lap
TEST_BIN := bin/lap-unit-tests

all: $(BIN)

$(BIN): $(SRC_LIB) $(SRC_MAIN) src/*.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(SRC_LIB) $(SRC_MAIN) $(LDFLAGS)

$(TEST_BIN): $(SRC_LIB) $(SRC_TESTS) src/*.h tests/unit/test.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC_LIB) $(SRC_TESTS) $(LDFLAGS)

unit: $(TEST_BIN)
	./$(TEST_BIN)

e2e: $(BIN)
	LAP="$(abspath $(BIN))" sh tests/e2e/run.sh

test: unit e2e

install: $(BIN)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/lap"
	@echo "installed $(DESTDIR)$(PREFIX)/bin/lap"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/lap"
	@echo "removed $(DESTDIR)$(PREFIX)/bin/lap"

clean:
	rm -rf bin

.PHONY: all unit e2e test install uninstall clean
