CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
LDLIBS  = -lm

all: ngsid_wav ngsid_test cpu_test c64_test psid_test sid_wav sid_play

ngsid_wav: tools/ngsid_wav.c ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tools/ngsid_wav.c ngsid.c $(LDLIBS)

ngsid_test: tests/test_synth.c ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_synth.c ngsid.c $(LDLIBS)

cpu_test: tests/test_cpu.c cpu6510.c cpu6510.h
	$(CC) $(CFLAGS) -o $@ tests/test_cpu.c cpu6510.c

# DORMANN=/pfad/6502_functional_test.bin make check-cpu
check-cpu: cpu_test
	./cpu_test $(DORMANN)

c64_test: tests/test_c64.c c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_c64.c c64.c cpu6510.c ngsid.c $(LDLIBS)

psid_test: tests/test_psid.c psid.c psid.h c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_psid.c psid.c c64.c cpu6510.c ngsid.c $(LDLIBS)

sid_wav: tools/sid_wav.c psid.c psid.h c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tools/sid_wav.c psid.c c64.c cpu6510.c ngsid.c $(LDLIBS)

sid_play: tools/sid_play.c psid.c psid.h c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tools/sid_play.c psid.c c64.c cpu6510.c ngsid.c $(LDLIBS)

sid_gui: tools/sid_gui.c gui/font_data.h psid.c psid.h c64.c c64.h cpu6510.c cpu6510.h ngsid.c ngsid.h
	$(CC) $(CFLAGS) -I. -o $@ tools/sid_gui.c psid.c c64.c cpu6510.c ngsid.c $$(pkg-config --cflags --libs sdl2) $(LDLIBS)

check: ngsid_test c64_test psid_test
	./ngsid_test
	./c64_test
	./psid_test

clean:
	rm -f sid_gui ngsid_wav ngsid_test cpu_test c64_test psid_test sid_wav sid_play
