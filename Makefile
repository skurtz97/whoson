# SPDX-License-Identifier: MIT OR Apache-2.0 OR BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Sean Kurtz
# shellcheck disable=SC2283
# ^ cleaners lint this file as shell; `NAME = value` is make syntax.
#
# whoson - build for z/OS with IBM Open XL C/C++ (ibm-clang).
#
# ZOAU is required: its shared library serves the opercmd display data.
# $(ZOAU_HOME)/lib/zoautil.x is the sidedeck, not the DLL: it is the import
# member the binder needs at link time. Override ZOAU_HOME on the command
# line if ZOAU lives elsewhere.
#
# 64-bit is not negotiable: the ZOAU shared library is a 64-bit DLL and both
# its sidedecks (/usr/lpp/IBM/zoautil/lib and /usr/lpp/IBM/zoau/v1r3/lib)
# import CODE64, so -m64 is required even though a 31-bit program would start
# faster. The compiler is pinned by absolute path because 2.1 and 2.2 are
# both installed and PATH has carried each; this project builds with 2.2
# (clang 21.1). A wrapper around the compiler that adds -Wl,dll,edit=no is
# unusable: it makes the binder treat the sidedeck as a DD and fail with
# IEW2763S. The library also has to be reachable at run time through LIBPATH.
#
# ASCII character mode is deliberate: the text ZOAU returns and the text the
# ssh channel carries are ASCII, so literals stay ASCII and the display fields
# compare the way they are parsed. Source stays ASCII-tagged.
#
# Every build is the release build: -O3, no -g, and a z/OS program object has
# no symbol table to remove. strings finds no function names in the binary,
# /bin/strip leaves its size alone, and the driver ignores -s as a
# compile-only flag.
#
# POSIX make syntax: z/OS has IBM's make as well as GNU make. Avoid ?=,
# which IBM's make does not accept; .PHONY is supported by both.

ZOAU_HOME = /usr/lpp/IBM/zoautil
PREFIX    = $(HOME)/.local
BINDIR    = $(PREFIX)/bin
CCDIR     = /usr/lpp/IBM/cnw/v2r2/openxl/bin
CC        = $(CCDIR)/ibm-clang
LISTFMT   = $(CCDIR)/ibm-clang-listing-formatter
CPPFLAGS  = -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200809L -D_ALL_SOURCE \
	    -D_EXT
CFLAGS    = -std=c17 -O3 -m64 -fzos-le-char-mode=ascii -Wall -Wextra \
	    -Wpedantic -Wshadow -Wconversion -I$(ZOAU_HOME)/include
ZOAU_DLL  = $(ZOAU_HOME)/lib/zoautil.x
CEEOPTS   = HEAPCHK(ON,1,32),RPTSTG(ON),FILETAG(AUTOCVT,AUTOTAG),POSIX(ON)

.PHONY: all install check list clean

all: whoson

whoson: whoson.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ whoson.c $(ZOAU_DLL)

# Open XL 2.2 can write a listing of the compile, which the formatter shipped
# with it renders as pseudo assembly with offsets, grouped by function.  The
# binary and the listing come out of the same compile, so they cannot
# disagree; whoson.json is the intermediate the formatter reads.
list:
	$(CC) $(CPPFLAGS) $(CFLAGS) -mzos-listing=json -mzos-listing-file-name=whoson.json -o whoson whoson.c $(ZOAU_DLL)
	$(LISTFMT) whoson.json > whoson.listing

# CEE's heap checker catches overruns; RPTSTG prints the storage report at
# exit, which is how leaks show up without a debugger.
# Installs into $(PREFIX)/bin, ~/.local/bin by default, which is on PATH
# ahead of everything else, so a rebuilt whoson is the one that runs.  Pass
# PREFIX to put it elsewhere: make PREFIX=/usr/local install
install: whoson
	mkdir -p $(BINDIR)
	cp whoson $(BINDIR)/whoson
	chmod +x $(BINDIR)/whoson

check: whoson
	LIBPATH=$(ZOAU_HOME)/lib:$$LIBPATH \
	    _CEE_RUNOPTS="$(CEEOPTS)" ./whoson

clean:
	rm -f whoson *.o whoson.json whoson.listing
