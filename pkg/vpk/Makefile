# vpk — the Verk package manager
PREFIX ?= /usr
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LIBS    = -lcurl -llzma -lz

vpk: vpk.c
	$(CC) $(CFLAGS) -o vpk vpk.c $(LIBS)

install: vpk
	install -D -m0755 vpk $(DESTDIR)$(PREFIX)/bin/vpk
	ln -sf vpk $(DESTDIR)$(PREFIX)/bin/verk

clean:
	rm -f vpk

.PHONY: install clean
