# Changelog

## 1.0.0

First release.

- Lists TSO/E sessions from `DISPLAY TS,A` and SSH sessions from
  `DISPLAY OMVS,A=ALL`, through the ZOAU shared library vector rather than a
  shell call to `opercmd`: user, session type, status, address space id, pid,
  job id, CPU time, elapsed time, service class, priority and region.
- `CPU` for an SSH session comes from `CT_SECS` on its own OMVS process; both
  it and the TSO/E `CT` print as seconds.milliseconds.
- `PROCS` counts the processes below the session's pid, since each z/OS fork
  gets its own address space.
- `USER` is the display's `USERID=`, which is not always the job name.
- Started tasks are dropped: the `sshd` listener process is recognised by its
  parent being init, and its daemon account is left out.
- Output leaves with `write(2)` in the character set the destination expects,
  and `--help` / `--version` / errors take the same path.
- Builds with IBM Open XL C/C++ 2.2 (clang 21.1), `-std=c17 -O3 -m64`, ASCII
  character mode; `make list` writes a compiler listing.
