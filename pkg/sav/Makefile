# savc — the sav compiler.
#
# A host program: it runs where you build it, and the executables it writes
# are aarch64 whatever it runs on. Objects go in a directory named after the
# host, because this repository is built from a Mac and from inside a Linux
# container against the same checkout, and one set of object files cannot
# serve both — a link that mixes them fails in a way that names neither.
HOST   := $(shell uname -s)-$(shell uname -m)
B      := build/$(HOST)
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
SRCS    = src/lex.c src/types.c src/parse.c src/check.c src/arm.c src/gen.c src/elf.c src/main.c
OBJS    = $(patsubst src/%.c,$(B)/%.o,$(SRCS))

all: savc

# Phony, so that the copy happens on every build: the file in the working
# directory belongs to whichever host built last, and the one under build/
# belongs to this one.
# Replaced rather than overwritten: on macOS a Mach-O binary modified in place
# has its signature invalidated and the next run dies with "Killed: 9", which
# looks like a compiler crash and is not one.
savc: $(B)/savc
	@cp -f $< $@.new && mv -f $@.new $@

$(B)/savc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(B)/%.o: src/%.c src/sav.h src/arm.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

test: savc
	@sh tests/run.sh

clean:
	rm -rf build savc examples/*.elf

.PHONY: all savc test clean
