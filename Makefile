# ngSid - NextGenSid
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
LDLIBS  = -lm
PREFIX  ?= /usr/local

SRC = psid.c c64.c cpu6510.c ngsid.c
HDR = psid.h c64.h cpu6510.h ngsid.h

all: ngSid

ngSid: gui/main.c gui/font_data.h $(SRC) $(HDR)
	$(CC) $(CFLAGS) -I. -Igui -o $@ gui/main.c $(SRC) $$(pkg-config --cflags --libs sdl2) $(LDLIBS)

install: ngSid
	install -Dm755 ngSid $(DESTDIR)$(PREFIX)/bin/ngSid

# self-tests (no SDL2 needed)
ngsid_test: tests/test_synth.c ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_synth.c ngsid.c $(LDLIBS)

cpu_test: tests/test_cpu.c cpu6510.c cpu6510.h
	$(CC) $(CFLAGS) -o $@ tests/test_cpu.c cpu6510.c

c64_test: tests/test_c64.c c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_c64.c c64.c cpu6510.c ngsid.c $(LDLIBS)

psid_test: tests/test_psid.c psid.c psid.h c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_psid.c psid.c c64.c cpu6510.c ngsid.c $(LDLIBS)

check: ngsid_test c64_test psid_test
	./ngsid_test
	./c64_test
	./psid_test

# DORMANN=/path/to/6502_functional_test.bin make check-cpu
check-cpu: cpu_test
	./cpu_test $(DORMANN)

clean:
	rm -f ngSid ngsid_test cpu_test c64_test psid_test

.PHONY: all install check check-cpu clean
