CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= $(HOME)/.local
ARCH    ?= x86_64
VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null || echo v0.0.0)

# The same source builds both ports; only the binary name differs.
ifeq ($(OS),Windows_NT)
BIN = aqua.exe
else
BIN = aqua
endif

DISTDIR = dist/aqua-$(VERSION)-linux-$(ARCH)
DISTTGZ = $(DISTDIR).tar.gz

all: $(BIN)

$(BIN): aqua.c
	$(CC) $(CFLAGS) -o $(BIN) aqua.c

ifneq ($(OS),Windows_NT)
tests/test_aqua: tests/test_aqua.c aqua.c
	$(CC) $(CFLAGS) -o tests/test_aqua tests/test_aqua.c

test: tests/test_aqua
	./tests/test_aqua

# A release tarball is built on Linux; on Windows ship the compiled aqua.exe,
# or run install-windows.ps1 to build and install it.
dist: $(BIN) tests/test_aqua
	rm -rf "$(DISTDIR)"
	mkdir -p "$(DISTDIR)/tests"
	cp $(BIN) aqua.c Makefile README.md LICENSE "$(DISTDIR)/"
	cp tests/test_aqua.c "$(DISTDIR)/tests/"
	tar czf "$(DISTTGZ)" -C dist "$(notdir $(DISTDIR))"
	rm -rf "$(DISTDIR)"
	@echo "built $(DISTTGZ)"
else
test:
	@echo "tests are POSIX-only; run 'make test' under Linux or WSL."

dist: $(BIN)
	@echo "built $(BIN) - package it for the Windows release (or run install-windows.ps1)."
endif

install: $(BIN)
	mkdir -p $(PREFIX)/bin
	cp $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f aqua aqua.exe tests/test_aqua
	rm -rf dist

.PHONY: all test install dist clean
