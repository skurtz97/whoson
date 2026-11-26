# Changelog

## Unreleased

- Rewritten without ZOAU and without a console display. Sessions are read
  straight out of common storage, walking CVT -> ASVT -> ASCB, which needs no
  APF authorization, no supervisor state and no key zero; process details come
  from `ps -A -o pid,jobname,user,time,args`, joined on the job name. The
  ZOAU shared library, `LIBPATH` and the `AC=1`/`extattr +a` install are gone.
- Only sessions are listed. Started tasks, OMVS daemons and the processes
  inside a session no longer appear as rows, so the table answers "who is
  logged on" rather than "what is running".
- `TYPE` is one of three: `TSO/E`, `z/OSMF` or `SSH`. `z/OSMF` is a TSO/E
  session whose logon procedure is the z/OSMF one (`IZUFPROC` by default, per
  the z/OSMF Configuration Guide's `COMMONPROC`/`FILEPROC`), which is how
  z/OSMF actually logs a user on; it is not a separate kind of session.
- `JOBNAME` and `LOGONPROC` are separate columns: `ASCBJBNI` is the job name,
  `ASCBJBNS` the name the address space was started with, which for TSO/E is
  the logon procedure.

## Unreleased

- `TYPE` says which kind of TSO/E logon a row is: `TSO/E ISPF` when the
  address space has no OMVS process, `TSO/E z/OSMF` when that process is a
  z/OSMF program (`IZUGDSPT`, a load module from the z/OSMF load library, in
  the `IZUFPROC` session z/OSMF logs on with) or when the CEA session table
  records a z/OSMF `APPID` for the session, and `TSO/E OMVS` for any other
  logon that has one. The z/OSMF session used to be reported as
  `TSO/E OMVS`.
- `F CEA,DIAG,SESSTABLE` is read as a third display, so a z/OSMF session is
  named even when it has no z/OSMF process left running. Losing that display
  costs the label, not the table, and is not reported.

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
