# ngSid - NextGenSid
#
# Playback uses libsidplayfp (cycle exact 6510/VIC-II/CIA, PSID and RSID) with ngSid's own
# VacSid style synth as the SID emulation. `make` downloads, verifies and builds libsidplayfp
# once (needs curl or wget, tar, a C++ compiler and make), then builds ngSid.
CC       ?= cc
CXX      ?= c++
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra
CXXFLAGS ?= -std=c++11 -O2 -Wall
LDFLAGS  ?= -static-libstdc++ -static-libgcc
LDLIBS   = -lm
PREFIX   ?= /usr/local

LSP_VER    = 2.16.1
LSP_SHA256 = ace0f73c2ef8645ab069ce1b298b10e31e36af7b5996109983b2b67ad60ff3ca
LSP_URL    = https://github.com/libsidplayfp/libsidplayfp/releases/download/v$(LSP_VER)/libsidplayfp-$(LSP_VER).tar.gz
LSP_TAR    = third_party/libsidplayfp-$(LSP_VER).tar.gz
LSP_SRC    = third_party/libsidplayfp-$(LSP_VER)
LSP_BUILD  = third_party/build
LSP_LIB    = $(LSP_BUILD)/src/.libs/libsidplayfp.a
LSP_INC    = -I$(LSP_SRC)/src -I$(LSP_BUILD)/src -I$(LSP_BUILD) -I$(LSP_SRC)/src/sidplayfp -I$(LSP_BUILD)/src/sidplayfp -DHAVE_CONFIG_H

OBJ_DIR  = build
SYNTH    = $(OBJ_DIR)/ngsid.o
PSIDINFO = $(OBJ_DIR)/psid.o
LSPOBJ   = $(OBJ_DIR)/engine.o $(OBJ_DIR)/ngsid-emu.o $(OBJ_DIR)/ngsid-builder.o
GUIOBJ   = $(OBJ_DIR)/main.o $(OBJ_DIR)/hvsc.o

all: ngSid

# ---- libsidplayfp (downloaded, verified, built statically) ---------------------------------
$(LSP_TAR):
	mkdir -p third_party
	if command -v curl >/dev/null 2>&1; then curl -fsSL -o $@.part $(LSP_URL); \
	else wget -q -O $@.part $(LSP_URL); fi
	echo "$(LSP_SHA256)  $@.part" | sha256sum -c - >/dev/null || { rm -f $@.part; echo "libsidplayfp checksum mismatch"; exit 1; }
	mv $@.part $@

$(LSP_LIB): $(LSP_TAR)
	rm -rf $(LSP_SRC) $(LSP_BUILD)
	tar -xzf $(LSP_TAR) -C third_party
	mkdir -p $(LSP_BUILD)
	cd $(LSP_BUILD) && ../libsidplayfp-$(LSP_VER)/configure --disable-shared --enable-static \
		--without-gcrypt --without-exsid --without-usbsid --disable-tests >/dev/null
	$(MAKE) -C $(LSP_BUILD) -j$$(nproc 2>/dev/null || echo 2) >/dev/null

deps: $(LSP_LIB)

# ---- objects ---------------------------------------------------------------------------------
$(OBJ_DIR):
	mkdir -p $@

$(SYNTH): ngsid.c ngsid.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ ngsid.c

$(PSIDINFO): psid.c psid.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ psid.c

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

$(OBJ_DIR)/version.h: FORCE | $(OBJ_DIR)
	@echo '#define NGSID_VERSION "$(VERSION)"' > $@.tmp; if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

FORCE:

$(OBJ_DIR)/main.o: gui/main.c gui/font_data.h gui/hvsc.h psid.h ngsid.h lsp/engine.h $(OBJ_DIR)/version.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -I. -Igui -I$(OBJ_DIR) $$(pkg-config --cflags sdl2) -c -o $@ gui/main.c

$(OBJ_DIR)/hvsc.o: gui/hvsc.c gui/hvsc.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -Igui -c -o $@ gui/hvsc.c

$(OBJ_DIR)/engine.o: lsp/engine.cpp lsp/engine.h lsp/ngsid-emu.h lsp/ngsid-builder.h ngsid.h $(LSP_LIB) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(LSP_INC) -I. -Ilsp -c -o $@ lsp/engine.cpp

$(OBJ_DIR)/ngsid-emu.o: lsp/ngsid-emu.cpp lsp/ngsid-emu.h ngsid.h $(LSP_LIB) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(LSP_INC) -I. -Ilsp -c -o $@ lsp/ngsid-emu.cpp

$(OBJ_DIR)/ngsid-builder.o: lsp/ngsid-builder.cpp lsp/ngsid-builder.h lsp/ngsid-emu.h ngsid.h $(LSP_LIB) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(LSP_INC) -I. -Ilsp -c -o $@ lsp/ngsid-builder.cpp

# ---- programs --------------------------------------------------------------------------------
ngSid: $(GUIOBJ) $(PSIDINFO) $(SYNTH) $(LSPOBJ) $(LSP_LIB)
	$(CXX) $(LDFLAGS) -o $@ $(GUIOBJ) $(PSIDINFO) $(SYNTH) $(LSPOBJ) $(LSP_LIB) $$(pkg-config --libs sdl2) $(LDLIBS)

install: ngSid
	install -Dm755 ngSid $(DESTDIR)$(PREFIX)/bin/ngSid
	install -Dm644 packaging/ngsid.desktop $(DESTDIR)$(PREFIX)/share/applications/ngsid.desktop
	install -Dm644 packaging/ngsid.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/ngsid.svg
	for n in 32 48 64 128 256 512; do \
		install -Dm644 packaging/icons/ngsid-$$n.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${n}x$${n}/apps/ngsid.png; \
	done

# ---- self-tests (no SDL2 needed) -----------------------------------------------------------
ngsid_test: tests/test_synth.c ngsid.c ngsid.h
	$(CC) $(CFLAGS) -o $@ tests/test_synth.c ngsid.c $(LDLIBS)

hvsc_test: tests/test_hvsc.c gui/hvsc.c gui/hvsc.h
	$(CC) $(CFLAGS) -o $@ tests/test_hvsc.c gui/hvsc.c

engine_test: tests/test_engine.c psid.h $(PSIDINFO) $(SYNTH) $(LSPOBJ) $(LSP_LIB)
	$(CC) $(CFLAGS) -I. -Ilsp -c -o $(OBJ_DIR)/test_engine.o tests/test_engine.c
	$(CXX) $(LDFLAGS) -o $@ $(OBJ_DIR)/test_engine.o $(PSIDINFO) $(SYNTH) $(LSPOBJ) $(LSP_LIB) $(LDLIBS)

check: ngsid_test hvsc_test engine_test
	./ngsid_test
	./hvsc_test
	./engine_test

clean:
	rm -rf $(OBJ_DIR) ngSid ngsid_test hvsc_test engine_test

distclean: clean
	rm -rf third_party

.PHONY: all deps install check clean distclean
