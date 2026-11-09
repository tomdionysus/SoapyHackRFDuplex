# Contributing

Read this, [README.md](README.md), [TODO/ACTIVE.md](TODO/ACTIVE.md)
(open work and standing decisions) and [docs/design.md](docs/design.md)
before starting. Do not reopen a standing decision in
[TODO/ACTIVE.md](TODO/ACTIVE.md) without saying so.

## Repository layout

```
docs/      design, licensing
module/    the SoapySDR module: src/ and its Catch2 tests in tests/
tools/     hackrfduplex-check, and ci.sh, the checks a change must pass
cmake/     version header, sanitizer and coverage builds, libhackrf lookup
debian/    Debian packaging: the module and the check tool
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
```

Inside `module/src`, everything that does not touch libhackrf or
SoapySDR is the core (`hackrfduplex_core`): formats and conversion, the
block ring, the gain and filter models, the two stream paths, the pair
and the board selection. It is tested against `tests/fake_board.*`.
`hackrf_board.*` is the one file that calls libhackrf; `soapy_device.*`
and `registration.cpp` are the ones that see SoapySDR, and they only
translate.

## Branches

- Work happens on `develop`. `main` receives `develop` at version tags.
- Isolated work is a branch off `develop`, `feature/<short-name>`,
  merged back and deleted.
- Tags are exactly `x.y.z`, annotated, no `v`, cut from `main`. The tag,
  `project(... VERSION ...)` in `CMakeLists.txt` and the module's
  `module_version` are one number.

## Code

- C++20, GNU extensions off, warnings are errors.
- Every source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.
  Code taken from a third-party work keeps its copyright and licence
  header beneath it.
- [.clang-format](.clang-format) is authoritative: Google base, four
  spaces, 100 columns. Headers are `.hpp`; types `PascalCase`, functions
  and variables `snake_case`, members `name_`, constants `kName`,
  namespace `hackrfduplex`.
- Logic that touches hardware sits behind the `Board` interface so the
  tests use the fake. New code without tests is not finished.
- Failures throw: `BoardError` for libhackrf, `std::invalid_argument` for
  a caller's mistake, `std::logic_error` for a call out of order. A read
  that finds nothing is a timeout, not an error.
- Prefer writing something by hand over adding a library, and say so
  explicitly when a dependency really is the right call.
- No emoji anywhere. No AI or tool attribution in code, comments or
  commit messages; the README's AI section is the one statement of it.

## Build and test

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

`tools/ci.sh` runs what a change must pass: the format check, the
licence line on every source file, the release build and its tests, and
the sanitizer build and its tests (`--quick` stops after the release
tests). Run it before a commit that touches code.

`cmake --preset asan` and `cmake --preset coverage` build the sanitizer
and coverage variants; `cmake --build --preset coverage --target
coverage` writes an lcov report to `build-coverage/coverage/html`.
Sanitizers run before a release or to hunt a bug that cannot be pinned
any other way.

Tests come from the design and the SoapySDR contract, never from the
current behaviour: write the test for what is required, watch it fail,
then write the code. A test is never deleted; a count that drops is a
finding.

The hardware check is `hackrfduplex-check` on two attached boards; what
it should report is the acceptance test of each hardware item in
[TODO/ACTIVE.md](TODO/ACTIVE.md).

## Safety

- No transmit test without the board owner's explicit permission for that
  test, asked at the time. Anything that activates the TX stream with
  samples that are not all zero is a transmit test. `hackrfduplex-check`
  transmits only with `--tx-tone`.
- The RF amplifier is never switched on by default, and never into an
  antenna without knowing what else is in range.
- A board that transmits is stopped when its stream is deactivated or
  closed, when the device is destroyed, and when the process ends.

## Commits

- One logical change per commit, subject in the imperative under 72
  characters, a body saying why when it is not obvious. No trailers.
- Never commit build output or anything from a board.

## Documentation

- Docs state current facts: what exists, how to install and use it, what
  was measured. No history, no plans; those belong in `TODO/`.
- No serials, hostnames or other identifiers of the development hardware;
  those live in the gitignored `CLAUDE.local.md`.
- [README.md](README.md) holds the overview, status, hardware, build,
  use, roadmap, AI statement and licence; everything else goes in
  `docs/`.
- [TODO/ACTIVE.md](TODO/ACTIVE.md) is the single list of open work, each
  item with an acceptance test. [TODO/COMPLETED.md](TODO/COMPLETED.md) is
  append-only at the top.
