# whoson

Who is logged on to this z/OS system, and how: TSO/E or SSH, with the job id,
CPU time, elapsed time, service class, priority and region size.

```
$ whoson
USER     TYPE   STATUS ASID WUID       CPU      ELAPSED    SCL  PRIO RGP
SKURTZ   SSH    -      0033 -          -        04.06.29   -    -    -
SKURTZ   TSO/E  OWT    0058 TSU08375   000.058  01.26.04   TSO  1    N/A
```

`TYPE` is `TSO/E` for a session listed by `DISPLAY TS,A`, `SSH` for a session
whose process is an `sshd`.

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

```
make            # builds ./whoson with ibm-clang, -m64, EBCDIC character mode
make check      # runs it under the CEE heap checker and storage report
make install    # copies it to ~/.local/bin
```

The sources are ASCII and stay that way; the compiler turns the literals into
EBCDIC, which is what ZOAU returns and what the console display looks like.

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
so the console command is passed quoted (`"D TS,A"`) as a single argument, in
EBCDIC. Unquoted, the `D` is read as an option letter and the call fails with
`opercmd`'s usage text and rc 2.

## shell/

`shell/whoson` is the earlier POSIX shell version. It needs `opercmd` on
`PATH` and the APF-authorized helpers behind it, but no compiler, so it is
useful on a system where the build is not.

## License

MIT OR Apache-2.0 OR BSD-2-Clause, at your option. See `LICENSE`.
