# whoson

Who is logged on to this z/OS system, and how: TSO/E or SSH, with the job id,
CPU time, elapsed time, service class, priority and region size.

```
$ whoson
USER     TYPE         STATUS ASID PID         WUID       CPU      ELAPSED    SCL  PRIO RGP
SKURTZ   SSH          -      0033 67109440    NA         NA       04.21.51   NA   NA   NA
SKURTZ   TSO/E OMVS   OWT O  0058 33554625    TSU08400   000.870  00.08.00   TSO  1    N/A
```

`TYPE` is `TSO/E` for a session listed by `DISPLAY TS,A`, `SSH` for a session
whose process is an `sshd`, and `TSO/E OMVS` for a TSO/E session that is also
using an OMVS process - a TSO/E logon you can reach the POSIX side from. `PID`
is the OMVS process id of that session, matched to the TSO/E session by
address space id: an SSH session always has one, a TSO/E session only while it
owns an OMVS process, and `NA` otherwise.

## Requirements

- z/OS with ZOAU installed at `/usr/lpp/IBM/zoautil` (v1.3 or later: the
  program talks to its shared library and needs the v4 vector table).
- IBM Open XL C/C++ (`ibm-clang`) for the build.
- `LIBPATH` must include `/usr/lpp/IBM/zoautil/lib` at run time, or the loader
  stops the program with `CEE3501S The module zoautil.so was not found`. The
  `Makefile` sets it for `make check`; `~/.zashrc` and `~/.ssh/environment` set
  it for interactive shells on the CBT system.

The build is 64-bit on purpose: the ZOAU shared library is a 64-bit DLL, so
its sidedeck imports `CODE64`.

## Build and run

```sh
make            # builds ./whoson with ibm-clang, -m64, ASCII character mode
make check      # runs it under the CEE heap checker and storage report
make install    # copies it to ~/.local/bin, creating it if needed
```

`PREFIX` defaults to `$(HOME)/.local`; pass your own to install elsewhere:

```sh
make PREFIX=/usr/local install   # -> /usr/local/bin/whoson
```

The sources are ASCII and stay that way; the build is in ASCII character mode
(`-fzos-le-char-mode=ascii`). ZOAU hands its display back in EBCDIC, so
`run_display` converts each response in place (`__e2a_l`) before it is parsed.

Output does not go through `printf`. `emit()` writes every message and the
table itself with `write(2)`, in the character set the destination expects:
plain ASCII when the descriptor is a terminal, EBCDIC when it is a pipe, a
socket or a file. CEE would convert stdio output from ASCII to EBCDIC on a
terminal, and this system's sshd converts the other way for a command it runs
without a tty, so neither `printf` nor a raw `write()` is correct on both
paths. Help and error text go through the same `emit()`, so `whoson -h` and
the usage message are readable whichever way they are run.

## How the data is produced

Both displays go through the ZOAU shared library vector (`vector->opercmd`),
so the program does not shell out to `opercmd`:

- `DISPLAY TS,A` lists TSO/E sessions: user, status, address space id, CPU
  time (`CT`), elapsed time (`ET`), job id (`WUID`), service class, priority
  and region (`RGP`). `DISPLAY TSO,...` is not a valid command on this system,
  and `DISPLAY TS,L` omits the timing fields, so `,A` is used.
- `DISPLAY OMVS,A=ALL` lists OMVS processes. An `sshd` process is a session;
  the account that owns the `sshd` process whose parent is the init process is
  a daemon account and is dropped, which keeps started tasks out of the list.

The vector's `opercmd` splits its argument string into `argv` without a shell,
so the console command is passed quoted (`"D TS,A"`) as a single argument.
Unquoted, the `D` is read as an option letter and the call fails with
`opercmd`'s usage text and rc 2.

## License

MIT OR Apache-2.0 OR BSD-2-Clause, at your option. See `LICENSE`.
