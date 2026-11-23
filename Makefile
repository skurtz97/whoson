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
# faster. Use the IBM compiler directly, not the ~/.local/bin/ibm-clang
# wrapper: the wrapper injects -Wl,dll,edit=no, which makes the binder treat
# the sidedeck as a DD and fail with IEW2763S. The library also has to be
# reachable at run time through LIBPATH.
#
# EBCDIC is deliberate: the ssh channel converts terminal output from EBCDIC
# to ASCII, and ZOAU returns its text in EBCDIC. Source stays ASCII-tagged;
# the compiler turns literals into EBCDIC. Only opercmd's argument string is
# converted to ASCII, in whoson.c.
#
# POSIX make syntax: z/OS has IBM's make as well as GNU make. Avoid ?=,
# which IBM's make does not accept; .PHONY is supported by both.

ZOAU_HOME = /usr/lpp/IBM/zoautil
PREFIX    = $(HOME)/.local
BINDIR    = $(PREFIX)/bin
CC        = /usr/lpp/IBM/cnw/v2r1/openxl/bin/ibm-clang
CPPFLAGS  = -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200809L -D_ALL_SOURCE \
	    -D_EXT
CFLAGS    = -std=c17 -O2 -m64 -fzos-le-char-mode=ebcdic -Wall -Wextra \
	    -Wpedantic -Wshadow -Wconversion -I$(ZOAU_HOME)/include
ZOAU_DLL  = $(ZOAU_HOME)/lib/zoautil.x
CEEOPTS   = HEAPCHK(ON,1,32),RPTSTG(ON),FILETAG(AUTOCVT,AUTOTAG),POSIX(ON)

.PHONY: all install check clean

all: whoson

whoson: whoson.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ whoson.c $(ZOAU_DLL)

# CEE's heap checker catches overruns; RPTSTG prints the storage report at
# exit, which is how leaks show up without a debugger.
# Installs into $(PREFIX)/bin, ~/.local/bin by default, which is on PATH
# ahead of everything else, so a rebuilt whoson is the one that runs.  Pass
# PREFIX to put it elsewhere: make PREFIX=/usr/local install
install: whoson
	mkdir -p $(BINDIR)
	cp whoson $(BINDIR)/whoson

check: whoson
	LIBPATH=$(ZOAU_HOME)/lib:$$LIBPATH \
	    _CEE_RUNOPTS="$(CEEOPTS)" ./whoson

clean:
	rm -f whoson *.o
