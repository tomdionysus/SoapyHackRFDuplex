# Contributing

Read this, [README.md](README.md), [TODO/ACTIVE.md](TODO/ACTIVE.md)
(open work and standing decisions) and [docs/design.md](docs/design.md)
before starting. Do not reopen a standing decision in
[TODO/ACTIVE.md](TODO/ACTIVE.md) without saying so.

## Repository layout

```
docs/      design, compatibility, examples, licensing
module/    the SoapySDR module: src/ and its Catch2 tests in tests/
tools/     hackrfduplex-check; ci.sh, the checks a change must pass; ci-pi.sh,
           which runs them on a Debian host with the boards attached
examples/  ft8: hackrfduplex-ft8, an FT8 client (optional build, liquid-dsp)
cmake/     version header, sanitizer and coverage builds, libhackrf lookup
debian/    Debian packaging: the module and the check tool
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
.github/   GitHub Actions: CI (tools/ci.sh in Debian trixie) and CodeQL
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

`tools/ci.sh` runs what a change must pass, in order:

1. the format check (clang-format 19) and the licence line on every
   source file;
2. the release build and its tests (`--quick` stops here);
3. the address and undefined-behaviour sanitizer build and its tests;
4. the build with the receive corrections (`HACKRFDUPLEX_DSP`, liquid-dsp)
   and its tests;
5. Clang's thread-safety analysis, a clang++-19 build where GCC is the
   compiler;
6. clang-tidy with [.clang-tidy](.clang-tidy), where the host has it;
7. the coverage report, where lcov is installed (`build-coverage/coverage/html`);
8. the thread sanitizer build and its tests, where the host can run it
   (not on a Raspberry Pi 5 kernel: its 47-bit address space is one
   ThreadSanitizer cannot use).

Run it before a commit that touches code. GitHub Actions runs the whole
of it on every push to `develop` and `main` in a Debian trixie container,
keeps the coverage report as an artifact and sends it to Codecov
(`CODECOV_TOKEN`, a repository secret), and runs CodeQL; the run on
the target, with the boards attached, stays the gate for a change.

The build of record runs on the target, a Debian trixie host with both
boards attached: `HACKRFDUPLEX_TARGET=user@host tools/ci-pi.sh` copies
the working tree there over ssh and runs `tools/ci.sh` in the copy, and
`tools/ci-pi.sh -- <command>` runs anything else there, such as
`hackrfduplex-check`. The target's address stays out of the repository.

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
  was measured. No history, no plans; those belong in `TODO/`, apart
  from [CHANGELOG.md](CHANGELOG.md), one short entry per release.
- No serials, hostnames or other identifiers of the development hardware;
  those live in the gitignored `CLAUDE.local.md`.
- [README.md](README.md) is the front page: what the project is, how to
  install it, where to start, the roadmap, the AI statement and the
  licence, kept short. Everything else goes in `docs/`, listed in
  [docs/index.md](docs/index.md).
- [TODO/ACTIVE.md](TODO/ACTIVE.md) is the single list of open work, each
  item with an acceptance test. [TODO/COMPLETED.md](TODO/COMPLETED.md) is
  append-only at the top.
