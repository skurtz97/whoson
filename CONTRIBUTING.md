# Contributing to whoson

Small tool, small rules.

## Building

whoson is a single C17 file with no external library, so it builds on z/OS
and nowhere else:

```sh
make            # -> ./whoson
make check      # under the CEE heap checker and storage report
make install    # -> ~/.local/bin/whoson
```

Needs IBM Open XL C/C++ 2.2 (`ibm-clang`, pinned by absolute path in the
`Makefile`) and z/OS 2.5 or later, the oldest target that compiler accepts.

GNU make and IBM's make both have to parse the Makefile: keep it to POSIX make
syntax (`?=` is out), and let `make -n` stay free of side effects.

## Style

- Warnings are bugs. The tree builds with `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion`; a warning from any of them, or from `clang-tidy` with
  `.clang-tidy`, gets fixed rather than silenced.
- `clang-format -i whoson.c` before committing; `.clang-format` is the
  authority, and CI checks the result against clang-format 23.1.5.
- Comments say why, not what, and the file header says what the control-block
  walk and the process table give and what they do not.

## Changes

- One PR per change, branched from `main`, merged through the PR.
- Touching how a control block or a `ps` row is parsed means saying which field
  was wrong and what the probe or `ps` showed. Do not adjust an example to make
  a claim true.
- No binaries in the tree, and nothing generated (`.clangd` indexes,
  `whoson.json`) gets committed.

## Releasing

1. Bump `VERSION` in the `Makefile` and add the entry to `CHANGELOG.md`.
2. Merge, then tag `main` with `v<version>` and push the tag.
3. `.github/workflows/release.yml` refuses a tag that disagrees with
   `VERSION`, takes the release notes from that `CHANGELOG.md` section, and
   attaches the source archive with its sha256.

There is no binary in a release: whoson only builds on z/OS, so a release
ships the sources and the build instructions.
