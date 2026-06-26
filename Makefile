CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= $(HOME)/.local

aqua: aqua.c
	$(CC) $(CFLAGS) -o aqua aqua.c

install: aqua
	mkdir -p $(PREFIX)/bin
	cp aqua $(PREFIX)/bin/aqua

clean:
	rm -f aqua

.PHONY: install clean
