# SPDX-License-Identifier: GPL-2.0-or-later
# crc16 - compute many CRC-16 variants of a file in a single pass
# Copyright (C) 2026 Daniel Dias Rodrigues <danieldiasr@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, see <https://www.gnu.org/licenses/>.

# Makefile for crc16 (GNU make). All build output goes to $(BUILD).

CC       ?= cc
STD      ?= c23
CFLAGS   ?= -O2
CPPFLAGS ?=
LDFLAGS  ?=
PREFIX   ?= /usr/local
DESTDIR  ?=
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
BUILD    ?= build
DEB_DIR  ?= $(BUILD)/deb
DEB_OPTS ?= -us -uc
SIMD     ?= 1

WARN     := -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes
THREADS  ?= -pthread
ifeq ($(SIMD),0)
SIMD_FLAGS := -DCRC16_NO_SIMD
endif
ALL_CFLAGS := -std=$(STD) $(THREADS) $(SIMD_FLAGS) $(WARN) $(CFLAGS) $(EXTRA_CFLAGS) -MMD -MP

LIB_SRCS := variants engine gf2 multi simd_x86 parallel cli output
LIB_OBJS := $(LIB_SRCS:%=$(BUILD)/obj/%.o)
MAIN_OBJ := $(BUILD)/obj/main.o
TEST_OBJ := $(BUILD)/obj/test_main.o
BENCH_OBJ := $(BUILD)/obj/bench.o

.PHONY: all clean check check-portable check-generic tsan bench install uninstall man std-check sanitize deb

all: $(BUILD)/crc16

$(BUILD)/crc16: $(MAIN_OBJ) $(LIB_OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/test_main: $(TEST_OBJ) $(LIB_OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/bench: $(BENCH_OBJ) $(LIB_OBJS)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -Isrc -c -o $@ $<

$(BUILD)/obj/test_main.o: tests/test_main.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -Isrc -c -o $@ $<

$(BUILD)/obj/bench.o: bench/bench.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -Isrc -c -o $@ $<

$(BUILD)/obj:
	mkdir -p $@

check: $(BUILD)/crc16 $(BUILD)/test_main
	$(BUILD)/test_main
	sh tests/ref_compare.sh $(BUILD)/crc16

# Same tests with the endian-independent byte-assembly loads (what a
# big-endian host uses).
check-portable:
	$(MAKE) BUILD=$(BUILD)/portable SIMD=0 check

check-generic:
	$(MAKE) BUILD=$(BUILD)/generic EXTRA_CFLAGS="$(EXTRA_CFLAGS) -DCRC16_GENERIC_LOAD" check

bench: $(BUILD)/bench
	$(BUILD)/bench

# Lint the man page (needs groff; mandoc is used too when installed).
man:
	groff -t -man -Tutf8 -ww -z man/crc16.1
	@if command -v mandoc >/dev/null 2>&1; then mandoc -Tlint man/crc16.1; fi

# Warnings are errors here; compile every source under each ISO C standard.
std-check:
	@for s in c99 c11 c17 c23; do \
	  echo "== -std=$$s"; \
	  for f in src/*.c tests/test_main.c bench/bench.c; do \
	    $(CC) -std=$$s $(THREADS) -pedantic-errors $(WARN) -Werror -O2 -Isrc -fsyntax-only $$f || exit 1; \
	  done; \
	done; echo "std-check: c99 c11 c17 c23 OK"

# ThreadSanitizer run of the whole test suite.
tsan:
	$(MAKE) BUILD=$(BUILD)/tsan CFLAGS="-O1 -g" \
	  EXTRA_CFLAGS="-Werror -fsanitize=thread -DCRC16_TSAN_PTHREAD" LDFLAGS="-fsanitize=thread" check

sanitize:
	$(MAKE) BUILD=$(BUILD)/san CFLAGS="-O1 -g -fno-omit-frame-pointer" \
	  EXTRA_CFLAGS="-Werror -fsanitize=address,undefined -fno-sanitize-recover=all" \
	  LDFLAGS="-fsanitize=address,undefined" check

# Build Debian packages (.deb with its -dbgsym, source package) in $(DEB_DIR).
# The orig tarball is made from this tree minus debian/ and build output;
# dpkg-buildpackage runs in a scratch copy, so the source tree stays clean.
# DEB_OPTS is passed to dpkg-buildpackage (default: unsigned; e.g. "-b").
deb:
	@command -v dpkg-buildpackage >/dev/null 2>&1 || \
	  { echo "make deb: dpkg-buildpackage not found (install dpkg-dev)" >&2; exit 1; }
	@set -e; \
	full=$$(dpkg-parsechangelog -S Version); \
	up=$${full%-*}; \
	hdr=$$(sed -n 's/^#define CRC16_VERSION "\(.*\)"/\1/p' src/crc16.h); \
	if [ "$$up" != "$$hdr" ]; then \
	  echo "make deb: debian/changelog upstream version ($$up) != CRC16_VERSION ($$hdr)" >&2; exit 1; \
	fi; \
	src=$(DEB_DIR)/crc16-$$up; \
	rm -rf $(DEB_DIR); mkdir -p $$src; \
	tar --exclude=./build --exclude=./$(BUILD) --exclude=./.git --exclude=./debian -cf - . | \
	  tar -C $$src -xf -; \
	tar --sort=name --owner=0 --group=0 --numeric-owner \
	  --mtime="@$$(dpkg-parsechangelog -S Timestamp)" \
	  -C $(DEB_DIR) -cJf $(DEB_DIR)/crc16_$$up.orig.tar.xz crc16-$$up; \
	cp -a debian $$src/debian; \
	(cd $$src && dpkg-buildpackage $(DEB_OPTS)); \
	echo "make deb: packages are in $(DEB_DIR)"; ls $(DEB_DIR)/*.deb

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BUILD)/crc16 $(DESTDIR)$(BINDIR)/crc16
	@if [ -f man/crc16.1 ]; then \
	  install -d $(DESTDIR)$(MANDIR)/man1; \
	  install -m 644 man/crc16.1 $(DESTDIR)$(MANDIR)/man1/crc16.1; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/crc16 $(DESTDIR)$(MANDIR)/man1/crc16.1

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/obj/*.d)
