# whoson

[![ci](https://github.com/skurtz97/whoson/actions/workflows/ci.yml/badge.svg)](https://github.com/skurtz97/whoson/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/skurtz97/whoson?sort=semver)](https://github.com/skurtz97/whoson/releases)
[![license](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0%20OR%20BSD--2--Clause-blue)](LICENSE)

Who is logged on to this z/OS system: the interactive sessions, one row each,
with the address space, the job name, the name the address space was started
with and its CPU time.

```text
$ whoson
USER     TYPE   ASID JOBNAME  LOGONPROC  PID      CPU
CBT002   TSO/E  0046 CBT002   CBT002     -        499:34:54
SKURTZ   SSH    002B SSHD16   -          83886753 4:53:34
SKURTZ   TSO/E  0060 SKURTZ   SKURTZ     -        68:54:38
```

Three types of session are listed, and nothing else:

- `TSO/E` — an address space with a TSO/E session control block (`ASCBTSB` is
  not zero). That covers a logon at a terminal, under ISPF or not, and it
  covers a z/OSMF user too, because z/OSMF logs its users on with a TSO/E
  logon procedure.
- `z/OSMF` — a `TSO/E` session whose logon procedure is the one z/OSMF is
  configured with. The z/OSMF Configuration Guide names it twice: `FILEPROC`
  for the data set and file REST services and `COMMONPROC` for the common TSO
  services, both defaulting to `IZUFPROC`, the procedure IBM ships in
  `SYS1.PROCLIB`. That is what separates a user who came in through the
  z/OSMF web interface from one who logged on at a terminal: same TSO/E
  machinery, different logon procedure.
- `SSH` — an `sshd` started with `-R`, which is the process sshd forks per
  connection (the listener runs without the flag). A connection that has not
  authenticated yet still runs under the daemon account and is not listed.

Started tasks, OMVS daemons and the processes inside a session are not
sessions and are not listed: `TCPIP`, `CEA`, `IZUSVR` and the shells behind an
`sshd` are all absent from the table by design.

`JOBNAME` is `ASCBJBNI`, the job name, and `LOGONPROC` is `ASCBJBNS`, the name
the address space was started with — for a TSO/E session that is the logon
procedure. `ASID` is the address space id, `PID` the POSIX process id where
one exists (`-` for a session that has none), and `CPU` is the sum of the
address space's TCB and SRB accumulator (`ASCBEJST` + `ASCBSRBT`, in
1024-microsecond units) as `hours:minutes:seconds`.

```sh
whoson            # the table
whoson --version  # whoson 1.0.0
whoson --help
```

## Requirements

- z/OS 2.1 or later.
- IBM Open XL C/C++ (`ibm-clang`) to build it.

There is nothing else: no ZOAU, no console access, no `LIBPATH`, and no
authorization. The program reads only common storage, which is mapped into
every address space.

## Install

Releases ship the sources, not a binary: whoson only builds on z/OS. Unpack a
release, build it, install it:

```sh
gh release download --repo skurtz97/whoson --pattern 'whoson-*'
sha256sum -c whoson-*.tar.gz.sha256
gzip -d whoson-*.tar.gz          # z/OS tar cannot gunzip one for you
pax -r -f whoson-*.tar
chtag -t -c ISO8859-1 -R whoson-*/   # pax untars as EBCDIC; make needs ASCII
cd whoson-*/ && make && make install
```

Without the `chtag`, IBM's make reads the Makefile as one line and stops with
`FSUM8705 No target`.

Or from a checkout, `make install` straight away. It copies the binary to
`~/.local/bin`; `PREFIX` moves it (`make PREFIX=/usr/local install`).

## Build and run

```sh
make            # builds ./whoson with ibm-clang, ASCII character mode
make check      # runs it under the CEE heap checker and storage report
make list       # builds, and writes a compiler listing to whoson.listing
make install    # copies it to ~/.local/bin, creating it if needed
```

`make list` turns on Open XL 2.2's listing (`-mzos-listing=json`) and renders
it with `ibm-clang-listing-formatter`: the pseudo assembly with offsets,
grouped by function, which is readable without a debugger.

Every build is the release build: `-O3` and no `-g`. A z/OS program object
carries no symbol table, so there is nothing to strip afterwards.

The sources are ASCII and stay that way; the build is in ASCII character mode
(`-fzos-le-char-mode=ascii`), which the 64-bit target is the only one to
accept. Names read out of control blocks arrive in EBCDIC and are converted
once, as they are copied.

Output does not go through `printf`. `emit()` writes every message and the
table itself with `write(2)`, in the character set the destination expects:
plain ASCII when the descriptor is a terminal, EBCDIC when it is a pipe, a
socket or a file. CEE converts stdio output from ASCII to EBCDIC on a terminal,
and this system's sshd converts the other way for a command it runs without a
tty, so neither `printf` nor a raw `write()` is right on both paths. Help and
error text go through the same `emit()`, so `whoson -h` and the usage message
are readable whichever way they are run.

## How the data is produced

No display command and no subsystem interface is involved:

- `CVT` is at `PSA+X'10'`, `CVTASVT` at `CVT+X'22C'`. The ASVT holds
  `ASVTMAXU` at `+X'204'` and the array of ASCB pointers at `+X'210'`; an
  entry with the high bit set is a free ASID, and `ASCB` at offset 0 proves
  the rest point at an address space control block.
- From each ASCB: `ASCBASID` (`X'24'`), `ASCBTSB` (`X'3C'`, the TSO/E test),
  `ASCBEJST` (`X'40'`) and `ASCBSRBT` (`X'C8'`) for CPU, `ASCBJBNI` (`X'AC'`)
  and `ASCBJBNS` (`X'B0'`) for the two names. The name pointers address common
  storage, so no cross-memory services are needed to follow them.
- `ps -A -o pid,jobname,user,time,args` supplies the pid, the user and the
  command. The job name joins a process to an address space; the command is
  what tells an `sshd` session (`-R`) from its listener.

The offsets above are the ones the ASCB and ASVT macros on the system
generate, and they are the ones the fields were read at when the walk was
checked against `DISPLAY TS`.

## Contributing

Build rules, style and the release steps are in `CONTRIBUTING.md`; what changed
in each release is in `CHANGELOG.md`.

## License

MIT OR Apache-2.0 OR BSD-2-Clause, at your option. See `LICENSE`.
