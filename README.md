# whoson

[![ci](https://github.com/skurtz97/whoson/actions/workflows/ci.yml/badge.svg)](https://github.com/skurtz97/whoson/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/skurtz97/whoson?sort=semver)](https://github.com/skurtz97/whoson/releases)
[![license](https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0%20OR%20BSD--2--Clause-blue)](LICENSE)

Who is logged on to this z/OS system: the interactive sessions, one row each,
with the address space, the job name, the logon procedure and its CPU time.

```text
$ whoson
USER     TYPE   ASID JOBNAME  LOGONPROC  PID      CPU
CBT002   TSO/E  0046 CBT002   CBT002     -        0:12:31
SKURTZ   z/OSMF 003D SKURTZ   IZUFPROC   -        0:03:41
SKURTZ   SSH    004C SKURTZ   -          33555303 0:00:02
```

Three types of session are listed, and nothing else:

- `TSO/E` — an address space with a TSO/E session control block (`ASCBTSB` is
  not zero) or a CSCB track id of X'01'. That covers a logon at a terminal,
  under ISPF or not, and it covers a z/OSMF user too, because z/OSMF logs its
  users on with a TSO/E logon procedure.
- `z/OSMF` — a `TSO/E` session whose logon procedure is the one z/OSMF is
  configured with. The z/OSMF Configuration Guide names it twice: `FILEPROC`
  for the data set and file REST services and `COMMONPROC` for the common TSO
  services, both defaulting to `IZUFPROC`, the procedure IBM ships in
  `SYS1.PROCLIB`. That is what separates a user who came in through the
  z/OSMF web interface from one who logged on at a terminal: same TSO/E
  machinery, different logon procedure. Any other procedure z/OSMF is
  configured with can be added to `WHOSON_ZOSMF_PROCS`.
- `SSH` — a process whose parent is an `sshd` (or `sshd-session`) and which is
  not itself an `sshd`. sshd forks that worker per connection; the listener
  has no `sshd` parent. A connection that has not authenticated yet is
  excluded by the same rule, and a port-forward-only connection has no such
  child, so neither is listed.

Started tasks, OMVS daemons and the processes inside a session are not
sessions and are not listed: `TCPIP`, `CEA`, `IZUSVR` and the shells behind an
`sshd` are all absent from the table by design.

`JOBNAME` is `ASCBJBNI` when it is set and `ASCBJBNS` otherwise — for a TSO/E
session `ASCBJBNI` is empty and `ASCBJBNS` holds the userid. `LOGONPROC` is
the session's CSCB class (`CHCLS`), which is the logon procedure for a TSO/E
user; `ASCBJBNS` only says how the address space was started. `USER` is the
CSCB key (`CHKEY`), the userid for a TSO/E session, and the POSIX effective
uid for an SSH session. `ASID` is the address space id, `PID` the POSIX
process id where one exists (`-` for a session that has none), and `CPU` is
TCB time (`ASCBEJST`) plus SRB time (`ASCBSRBT`) plus additional preemptable
SRB time (`ASSBASST`) as `hours:minutes:seconds`. All three are TOD-clock
accumulators, so the low 12 bits are the fraction, not 1024-microsecond
units.

```sh
whoson            # the table
whoson --version  # whoson 1.0.0
whoson --help
```

## Requirements

- z/OS 2.5 or later (the oldest target Open XL 2.2 accepts).
- IBM Open XL C/C++ (`ibm-clang`) to build it.

There is nothing else: no ZOAU, no console access, no `LIBPATH`, and no APF
authorization. The TSO/E half reads only common storage, which is mapped into
every address space. The SSH half calls `w_getpsent()`, which is filtered by
the caller's authority: without READ on `SUPERUSER.PROCESS.GETPSENT` (or an
equivalent uid match) other users' processes are invisible, so their SSH
sessions are missing from the table. TSO/E rows are unaffected.

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

Output does not go through `printf`, and it does not guess from `isatty()`.
The source and the program are ISO8859-1 (CCSID 819). `emit()` looks at the
destination's tag and sets the descriptor up the way [zopen][zopen] does:
a text file is tagged ISO8859-1 and receives ASCII (an untagged file is
tagged 819 first, so `whoson > out` produces a tagged ASCII file); a terminal,
a pipe or an EBCDIC-tagged file is written as IBM-1047 and the C runtime
converts the ASCII writes to EBCDIC through `fcntl(F_CONTROL_CVT)`. The
descriptor's saved setting is restored before exit, so the shell's own output
is not affected. That works the
same whether `_BPXK_AUTOCVT` is on or off, and on both a tty and a pipe,
where `printf` and a raw `write()` each get one of the two wrong. Help and
error text go through the same `emit()`.

[zopen]: https://zopen.community/Guides/CommonSolutions

## How the data is produced

No display command and no subsystem interface is involved:

- `CVT` is at `PSA+X'10'`, `CVTASVT` at `CVT+X'22C'`. The ASVT holds
  `ASVTMAXU` at `+X'204'` and the array of ASCB pointers at `+X'210'`; an
  entry with the high bit set is a free ASID, and `ASCB` at offset 0 proves
  the rest point at an address space control block.
- From each ASCB: `ASCBASID` (`X'24'`), `ASCBTSB` (`X'3C'`), `ASCBEJST`
  (`X'40'`) and `ASCBSRBT` (`X'C8'`) for CPU, `ASCBJBNI` (`X'AC'`) and
  `ASCBJBNS` (`X'B0'`) for the names. The name pointers address common
  storage, so no cross-memory services are needed to follow them. The ASCB's
  ACB eye-catcher and `ASCBASID == ASID` validate the block; a fault while
  reading any of it skips that row instead of ending the program.
- The session's CSCB is at `ASCB+X'38'`: `CHKEY` (`X'08'`, the userid),
  `CHCLS` (`X'10'`, the logon procedure) and `CHTRKID` (`X'1C'`, X'01' for a
  TSO/E user). `ASSBASST` (`X'160'` from `ASCBASSB`, `X'150'`) adds
  preemptable SRB time to the CPU column.
- `w_getpsent()` supplies the process tree, its parent and the executable
  path; a process whose parent is `sshd` and which is not `sshd` itself is an
  SSH session. `ps -A -o pid,xasid` supplies the address space id (the
  `W_PSPROC` interface has no ASID field), and the join to the ASCB is by
  ASID because sshd's per-connection job names repeat.

## Limits

- A z/OSMF login that never starts a TSO/E address space (some REST-only
  flows live entirely inside `IZUSVR1`) is not a TSO/E session and is not
  listed. A TSO/E session started by another CEA application can only be
  recognised by its procedure name, so it may be reported as `TSO/E`.
- SSH rows need `w_getpsent()` visibility, as described under Requirements.
  Only pty logins have a `/etc/utmpx` record; whoson does not read it, so a
  session's tty and remote address are not shown.

The offsets above are the ones the ASVT, ASCB, CSCB and ASSB macros on the
system generate, and they were checked against a probe that read all 355
address spaces on CBT.

## Contributing

Build rules, style and the release steps are in `CONTRIBUTING.md`; what changed
in each release is in `CHANGELOG.md`.

## License

MIT OR Apache-2.0 OR BSD-2-Clause, at your option. See `LICENSE`.
