# Changelog

## Unreleased

- Rewritten without ZOAU and without a console display. Sessions are read
  straight out of common storage, walking CVT -> ASVT -> ASCB, which needs no
  APF authorization, no supervisor state and no key zero; SSH sessions come
  from `w_getpsent()` and their address space id from `ps -o pid,xasid`. The
  ZOAU shared library, `LIBPATH` and the `AC=1`/`extattr +a` install are gone.
- Only sessions are listed. Started tasks, OMVS daemons and the processes
  inside a session no longer appear as rows, so the table answers "who is
  logged on" rather than "what is running".
- `TYPE` is one of three: `TSO/E`, `z/OSMF` or `SSH`. `z/OSMF` is a TSO/E
  session whose logon procedure is the z/OSMF one (`IZUFPROC` by default, per
  the z/OSMF Configuration Guide's `COMMONPROC`/`FILEPROC`), which is how
  z/OSMF actually logs a user on; it is not a separate kind of session.
- `JOBNAME` and `LOGONPROC` are separate columns: `ASCBJBNI` is the job name
  (falling back to `ASCBJBNS` when it is blank), and the logon procedure comes
  from the session's CSCB (`CHCLS`), not from `ASCBJBNS`.
- `CPU` adds `ASSBASST` to `ASCBEJST` + `ASCBSRBT` and converts TOD units with
  `>> 12`; the old rewrite multiplied by 1024, overstating CPU time by 2^22.
- `SSH` rows come from the process tree (parent `sshd`, child not `sshd`), not
  from `-R`, so OpenSSH 9.8's `sshd-session` and unauthenticated preauth
  children are handled. Rows join to the ASCB by ASID.
- The ASVT walk sizes itself from `ASVTMAXU`, validates each ASCB, and a fault
  while reading a terminated address space skips that row instead of abending.
- Output follows the zopen character-set convention: files are tagged
  ISO8859-1 (CCSID 819) and receive ASCII; terminals, pipes and EBCDIC files
  are IBM-1047 with the runtime converting from ASCII (`F_CONTROL_CVT`). The
  same tag drives how `ps` output is read.
- Requires z/OS 2.5 or later. Revises the tooling docs, CI comment and issue
  templates to drop ZOAU and the old console displays.

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
