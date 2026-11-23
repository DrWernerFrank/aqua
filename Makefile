CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= $(HOME)/.local
ARCH    ?= x86_64
VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null || echo v0.0.0)
DISTDIR  = dist/aqua-$(VERSION)-linux-$(ARCH)
DISTTGZ  = dist/aqua-$(VERSION)-linux-$(ARCH).tar.gz

aqua: aqua.c
	$(CC) $(CFLAGS) -o aqua aqua.c

tests/test_aqua: tests/test_aqua.c aqua.c
	$(CC) $(CFLAGS) -o tests/test_aqua tests/test_aqua.c

test: tests/test_aqua
	./tests/test_aqua

install: aqua
	mkdir -p $(PREFIX)/bin
	cp aqua $(PREFIX)/bin/aqua

dist: aqua tests/test_aqua
	rm -rf "$(DISTDIR)"
	mkdir -p "$(DISTDIR)/tests"
	cp aqua aqua.c Makefile README.md LICENSE "$(DISTDIR)/"
	cp tests/test_aqua.c "$(DISTDIR)/tests/"
	tar czf "$(DISTTGZ)" -C dist "$(notdir $(DISTDIR))"
	rm -rf "$(DISTDIR)"
	@echo "built $(DISTTGZ)"

clean:
	rm -f aqua tests/test_aqua
	rm -rf dist

.PHONY: test install dist clean
