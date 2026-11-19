# SPDX-FileCopyrightText: 2026 Nahuel Genchi
# SPDX-License-Identifier: GPL-2.0-or-later
APP_ID  = com.nahuelgenchi.Lares
ADDON   = lares@nahuelgenchi.github.io
PREFIX ?= $(HOME)/.local
ADDONS ?= $(HOME)/.local/share/gnome-shell/extensions
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter
GTK     = $(shell pkg-config --cflags gtk4 gio-unix-2.0)
GTK_LIBS = $(shell pkg-config --libs gtk4 gio-unix-2.0) -lm
GIO     = $(shell pkg-config --cflags gio-2.0 gio-unix-2.0)
GIO_LIBS = $(shell pkg-config --libs gio-2.0 gio-unix-2.0) -lm

PAD_SOURCES = $(wildcard src/pad/*.c)
AIRDROP_SOURCES = $(wildcard src/airdrop/*.c)
APP_SOURCES = $(wildcard src/*.c) $(PAD_SOURCES) $(AIRDROP_SOURCES) build/resources.c
HELPER_SOURCES = src/helper/lares-pad.c $(PAD_SOURCES)
HEADERS = $(wildcard src/*.h src/pad/*.h src/airdrop/*.h)
AWDL_SOURCES = $(wildcard src/awdl/*.c)
# What the AWDL tests need: frames and the node, without the radio or main().
AWDL_PURE = $(filter-out src/awdl/main.c src/awdl/radio.c,$(AWDL_SOURCES))

all: build/lares build/lares-pad build/lares-awdl

build/resources.c: data/lares.gresource.xml data/style.css data/pad/controller.txt
	@mkdir -p build
	glib-compile-resources --sourcedir=data --generate-source --target=$@ $<

build/lares: $(APP_SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(GTK) -o $@ $(APP_SOURCES) $(GTK_LIBS)

# The helper links GLib alone: no GTK, so it stays a few megabytes.
build/lares-pad: $(HELPER_SOURCES) $(wildcard src/pad/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) $(GIO) -o $@ $(HELPER_SOURCES) $(GIO_LIBS)

# AirDrop's link links libc alone: it holds two network capabilities, so it
# carries as little code as it can (docs/AIRDROP.md).
build/lares-awdl: $(AWDL_SOURCES) $(wildcard src/awdl/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(AWDL_SOURCES)

install: all
	install -Dm755 -s build/lares $(PREFIX)/bin/lares
	install -Dm755 -s build/lares-pad $(PREFIX)/bin/lares-pad
	@# Without capabilities here: Share's Allow installs a root-owned copy with them.
	install -Dm755 -s build/lares-awdl $(PREFIX)/libexec/lares-awdl
	install -Dm644 data/$(APP_ID).desktop $(PREFIX)/share/applications/$(APP_ID).desktop
	install -Dm644 data/$(APP_ID).svg $(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg
	@mkdir -p $(PREFIX)/share/systemd/user
	sed 's|@BINDIR@|$(PREFIX)/bin|' data/lares-pad.service.in > $(PREFIX)/share/systemd/user/lares-pad.service
	install -Dm644 -t $(ADDONS)/$(ADDON) data/addon/metadata.json data/addon/extension.js \
		data/addon/controllerArt.js data/addon/stylesheet.css data/pad/controller.txt
	install -Dm644 -t $(ADDONS)/$(ADDON)/icons data/addon/icons/lares-pad-symbolic.svg
	@# Padlight's photo-style picture, which earlier versions installed
	rm -rf $(ADDONS)/$(ADDON)/images
	@# Monocraft, Lares' lettering, unless it's installed already (SIL OFL 1.1)
	@if ! fc-list | grep -qi monocraft; then \
		install -Dm644 -t $(PREFIX)/share/fonts/lares data/fonts/Monocraft.ttf data/fonts/OFL.txt; \
		fc-cache -f $(PREFIX)/share/fonts/lares; fi
	-gtk4-update-icon-cache -q -t -f $(PREFIX)/share/icons/hicolor
	-update-desktop-database -q $(PREFIX)/share/applications

# What the Checks workflow runs: both programs with warnings as errors, the
# unit tests, and the addon's JavaScript parsed and its picture tested.
check: build/resources.c
	@mkdir -p build/check
	$(CC) $(CFLAGS) -Werror $(GTK) -o build/check/lares $(APP_SOURCES) $(GTK_LIBS)
	$(CC) $(CFLAGS) -Werror $(GIO) -o build/check/lares-pad $(HELPER_SOURCES) $(GIO_LIBS)
	$(CC) $(CFLAGS) -Werror $(GIO) -o build/check/unit tests/unit.c src/devices.c src/hyperx.c src/hidpp.c \
		src/pad/pads.c src/pad/padconf.c src/pad_map.c src/remote_model.c $(GIO_LIBS)
	build/check/unit
	$(CC) $(CFLAGS) -Werror -o build/check/lares-awdl $(AWDL_SOURCES)
	$(CC) $(CFLAGS) -Werror $(GIO) -o build/check/awdl tests/awdl.c $(AWDL_PURE) $(GIO_LIBS)
	build/check/awdl
	$(CC) $(CFLAGS) -Werror $(GIO) -o build/check/formats tests/formats.c $(AIRDROP_SOURCES) $(GIO_LIBS)
	build/check/formats
	$(CC) $(CFLAGS) -Werror $(GIO) -o build/check/discovery tests/discovery.c $(AIRDROP_SOURCES) $(GIO_LIBS)
	build/check/discovery
	$(CC) $(CFLAGS) -Werror $(GIO) -o build/check/share tests/share.c src/share.c $(AIRDROP_SOURCES) $(GIO_LIBS)
	build/check/share
	@for f in data/addon/*.js; do node --check "$$f" || exit 1; done
	node tests/addon.mjs
	@echo "make check: all passed"

uninstall:
	-systemctl --user disable --now lares-pad.service
	rm -f $(PREFIX)/bin/lares $(PREFIX)/bin/lares-pad $(PREFIX)/libexec/lares-awdl \
	      $(PREFIX)/share/applications/$(APP_ID).desktop \
	      $(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg $(PREFIX)/share/systemd/user/lares-pad.service
	rm -rf $(ADDONS)/$(ADDON) $(PREFIX)/share/fonts/lares
	@test ! -e /usr/local/libexec/lares-awdl || echo "AirDrop's link stays installed; remove it with:" \
		"sudo rm /usr/local/libexec/lares-awdl /etc/NetworkManager/conf.d/90-lares-airdrop.conf" \
		"/etc/systemd/network/10-lares-awdl.link"

clean:
	rm -rf build

.PHONY: all check install uninstall clean
