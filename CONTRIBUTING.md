# Contributing

Read this, [README.md](README.md), [TODO/ACTIVE.md](TODO/ACTIVE.md)
(open work and standing decisions) and [docs/design.md](docs/design.md)
before starting. Do not reopen a standing decision in
[TODO/ACTIVE.md](TODO/ACTIVE.md) without saying so.

## Reporting a problem

Bugs and requests go in the issue tracker,
<https://github.com/tomdionysus/SoapyHackRFDuplex/issues>, which is
public and keeps every report. Say which version (`SoapySDRUtil --info`
prints the module's), the host and its distribution, the boards and their
firmware (`hackrfduplex-check --probe`), what you ran and what you saw;
the output of `hackrfduplex-check` helps most. Reports are answered
within two weeks. A vulnerability is reported privately instead, as
[SECURITY.md](SECURITY.md) says.

Contributions come as pull requests against `develop`, following the
rest of this file: tests first, `tools/ci.sh` passing, one logical change
per commit.

## Repository layout

```
docs/      design, compatibility, examples, licensing
module/    the SoapySDR module: src/ and its Catch2 tests in tests/
tools/     hackrfduplex-check; ci.sh, the checks a change must pass; ci-pi.sh,
           which runs them on a Debian host with the boards attached;
           deb-*.sh, which build and check the Debian packages
examples/  ft8: hackrfduplex-ft8, an FT8 client (optional build, liquid-dsp)
fuzz/      libFuzzer targets; .clusterfuzzlite/ builds them for CI
cmake/     version header, sanitizer and coverage builds, libhackrf lookup
debian/    Debian packaging: the module and the check tool
TODO/      ACTIVE.md (open work) and COMPLETED.md (done work)
.github/   GitHub Actions: CI (tools/ci.sh in Debian trixie), CodeQL, Fuzz,
           Scorecard and Release; Dependabot
```

Inside `module/src`, everything that does not touch libhackrf or
SoapySDR is the core (`hackrfduplex_core`): formats and conversion, the
block ring, the gain and filter models, the two stream paths, the pair
and the board selection. It is tested against `tests/fake_board.*`.
`hackrf_board.*` is the one file that calls libhackrf; `soapy_device.*`
and `registration.cpp` are the ones that see SoapySDR, and they only
translate.

## Branches

- Work happens on `develop`. `main` receives `develop` at version tags,
  through a pull request whose CI and CodeQL checks have passed: `main`
  is protected, with no direct push, force push or deletion.
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

1. the format check (clang-format 19), the licence line on every
   source file, and ShellCheck over the scripts where the host has it;
2. the release build and its tests (`--quick` stops here);
3. the address and undefined-behaviour sanitizer build and its tests;
4. the build with the receive corrections (`HACKRFDUPLEX_DSP`, liquid-dsp)
   and its tests;
5. Clang's thread-safety analysis, a clang++-19 build where GCC is the
   compiler;
6. clang-tidy with [.clang-tidy](.clang-tidy), where the host has it;
7. the coverage report and its floors, where lcov is installed
   (`build-coverage/coverage/html`);
8. the fuzz targets, built by `tools/fuzz-build.sh` and each run for
   `FUZZ_SECONDS` (default 30), where Clang has libFuzzer;
9. the thread sanitizer build and its tests, where the host can run it
   (not on a Raspberry Pi 5 kernel: its 47-bit address space is one
   ThreadSanitizer cannot use).

Run it before a commit that touches code. GitHub Actions runs the whole
of it on every push to `develop` and `main` in a Debian trixie container,
keeps the coverage report as an artifact and sends it to Codecov
(`CODECOV_TOKEN`, a repository secret; [codecov.yml](codecov.yml) holds
its checks), checks the workflows with actionlint, and runs CodeQL with
its security-and-quality queries. It also builds the Debian packages for
amd64 and arm64 (`tools/deb-build.sh`, lintian failing on any warning)
and installs each into a clean container, where SoapySDR must load the
module (`tools/deb-install-check.sh`). Every action is pinned to a
commit, its release named beside it, and Dependabot proposes updates.
OpenSSF Scorecard rates the repository weekly. `main` takes only a pull
request that has passed CI and CodeQL. The run on the target, with the boards
attached, stays the gate for a change.

The fuzz targets in `fuzz/` take arbitrary input into what an
application or the bus hands the module: the format, clock and time
names, the device arguments and the boards listed, samples in every
format, and captures for the burst search. ClusterFuzzLite
(`.clusterfuzzlite/`, the Fuzz workflow) runs them ten minutes on each
push and pull request and an hour weekly; for longer, build them with
`tools/fuzz-build.sh` on a build machine and run them there. A crash a
fuzzer finds becomes a unit test before it is fixed. The sanitizer build
names `float-cast-overflow`, which GCC's `undefined` leaves out.

Coverage counts the module, the libhackrf binding (tested against a fake
libhackrf, `module/tests/fake_libhackrf.*`) and the FT8 example's own
code; it leaves out the tests, the SoapySDR registration, the check tool
and the FT8 client's main loop, which only SoapySDR's loader, the boards
or a person run. Branches are counted as the source has them, without the
compiler's exception and clean-up edges. `tools/ci.sh` fails below 99.5%
of lines, 99% of functions or 93% of branches. A line no input can reach,
such as the return after a `switch` that returns for every value, is
marked `// LCOV_EXCL_LINE: why`; nothing else is excluded by marker.

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

Several tests run the pair against a simulated link that moves a transfer
a millisecond, and some of what they check, such as a burst landing at
its time, depends on threads being scheduled within a few milliseconds.
They pass on an idle host and under a parallel `ctest -j`; a host pinned
by other work can still fail one now and then (about one run in ten with
six busy loops beside the suite). Such a failure is rerun on a quiet host
before it is taken as a finding.

## Releases

A release is a version number, a tag, a GitHub Release, and packages for
amd64 and arm64 in the project's apt repository. Pushing the tag starts
the Release workflow, which builds both architectures' packages from the
tag, installs them, and publishes the Release with the tag's
CHANGELOG.md entry as its notes. In order:

1. On `develop`: the version in `project(... VERSION ...)` in
   `CMakeLists.txt`, an entry at the top of [CHANGELOG.md](CHANGELOG.md),
   and a matching entry at the top of `debian/changelog`. One commit,
   `Version x.y.z`.
2. `tools/ci.sh` passes on a Debian trixie arm64 host (a build machine,
   or the target through `tools/ci-pi.sh`), and GitHub Actions passes on
   `develop`, the package jobs included.
3. Open a pull request from `develop` to `main` and merge it with a merge
   commit once its CI and CodeQL checks pass; `main` takes nothing else.
   Tag `x.y.z` on that merge commit (annotated, no `v`) and push the tag.
4. The Release workflow builds the packages from the tag, lintian free of
   errors and warnings, installs them and attaches them to the Release.
   Those files are the release's packages: download them
   (`gh release download x.y.z`) for the steps below, never build from a
   working tree. `tools/deb-amd64.sh x.y.z` builds the amd64 ones the
   same way locally, should GitHub be unavailable. Built from the tag's
   source archive (`git archive`) in `debian:trixie`, the packages are
   bit-for-bit the same each time (checked for 0.2.1); a build inside a
   git checkout differs, as it embeds the commit hash.
5. On the target, install the arm64 packages and run the receive smoke
   test and, with transmit allowed, the timed bursts (TODO/ACTIVE.md,
   "Resume here"); then remove the packages, which would otherwise
   shadow the build tree's module.
6. Put the packages in the apt repository and sign it, following that
   repository's own README. Before uploading, install from a copy of
   the signed tree in clean `debian:trixie` containers for both
   architectures; after uploading, install from the live repository the
   way the README says, again for both.

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
