# SPDX-License-Identifier: MIT OR Apache-2.0 OR BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Sean Kurtz
# shellcheck disable=SC2283
# ^ cleaners lint this file as shell; `NAME = value` is make syntax.
#
# whoson - build for z/OS with IBM Open XL C/C++ (ibm-clang).
#
# No external library: session data is read straight out of common storage
# (CVT -> ASVT -> ASCB), which needs no authorization, so there is no ZOAU
# import member and no LIBPATH to set at run time.
#
# -m64 is here for one reason: the ASCII character mode below is only
# accepted by the 64-bit target. The default 31-bit target is standard
# linkage (s390-ibm-zos-stdlink) and rejects it with "option
# '-fzos-le-char-mode=ascii' is incompatible with target triple". Nothing in
# the source needs 64 bits - the control block pointers are 32-bit - so if
# the ASCII mode is ever dropped, -m64 goes with it.
#
# ASCII character mode is deliberate: ps output and the ssh channel are
# ASCII, so literals stay ASCII and the names copied out of EBCDIC storage
# are converted once, in copy_name(). Source stays ASCII-tagged.
#
# Every build is the release build: -O3, no -g, and a z/OS program object has
# no symbol table to remove.
#
# POSIX make syntax: z/OS has IBM's make as well as GNU make. Avoid ?=,
# which IBM's make does not accept; .PHONY is supported by both.

# ~/.local/bin is enough now that the program is not authorized; the install
# target stays overridable for a system-wide /usr/local/sbin copy.
PREFIX    = $(HOME)/.local
BINDIR    = $(PREFIX)/bin
# The release workflow refuses a tag that does not match this, and the binary
# reports it through --version.
VERSION   = 1.0.0
CCDIR     = /usr/lpp/IBM/cnw/v2r2/openxl/bin
CC        = $(CCDIR)/ibm-clang
LISTFMT   = $(CCDIR)/ibm-clang-listing-formatter
CPPFLAGS  = -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200809L -D_ALL_SOURCE \
	    -D_EXT -DWHOSON_VERSION=\"$(VERSION)\"
CFLAGS    = -std=c17 -O3 -m64 -fzos-le-char-mode=ascii -Wall -Wextra \
	    -Wpedantic -Wshadow -Wconversion
CEEOPTS   = HEAPCHK(ON,1,32),RPTSTG(ON),FILETAG(AUTOCVT,AUTOTAG),POSIX(ON)

.PHONY: all install check list clean

all: whoson

whoson: whoson.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ whoson.c

# Open XL 2.2 can write a listing of the compile, which the formatter shipped
# with it renders as pseudo assembly with offsets, grouped by function.  The
# binary and the listing come out of the same compile, so they cannot
# disagree; whoson.json is the intermediate the formatter reads.
list:
	$(CC) $(CPPFLAGS) $(CFLAGS) -mzos-listing=json -mzos-listing-file-name=whoson.json -o whoson whoson.c
	$(LISTFMT) whoson.json > whoson.listing

# CEE's heap checker catches overruns; RPTSTG prints the storage report at
# exit, which is how leaks show up without a debugger.
install: whoson
	mkdir -p $(BINDIR)
	cp whoson $(BINDIR)/whoson
	chmod 755 $(BINDIR)/whoson

check: whoson
	_CEE_RUNOPTS="$(CEEOPTS)" ./whoson

clean:
	rm -f whoson *.o whoson.json whoson.listing
