# Governance

hackrfduplex is a one-maintainer project: Tom Cully
([@tomdionysus](https://github.com/tomdionysus)) wrote it, maintains it
and decides what goes into it.

## Roles

**Maintainer** (Tom Cully). Sets the project's scope and direction,
reviews and merges changes, cuts releases, runs the hardware checks on
the boards, answers issues and vulnerability reports, and holds the
repository's administration, the Codecov and OpenSSF registrations, and
the key that signs the tomcully.com apt repository.

**Contributor**: anyone who reports a problem, asks for something, or
sends a change. Contributions follow [CONTRIBUTING.md](CONTRIBUTING.md);
they are welcome, and are reviewed by the maintainer.

## How decisions are made

- What the project is for, and what it is not, is set in
  [TODO/ACTIVE.md](TODO/ACTIVE.md), "Standing decisions": a pair of
  HackRF One boards as one full-duplex SoapySDR device, working with
  stock firmware and libhackrf. A proposal that changes a standing
  decision is discussed in an issue first.
- Anything else is decided in the issue or pull request that raises it,
  in public. The maintainer has the final say and gives the reason.
- A change lands on `develop` once its tests pass in CI. `main` receives
  `develop` at each release, through a pull request that CI, CodeQL and
  fuzzing must pass ([CONTRIBUTING.md](CONTRIBUTING.md), Releases).

## Security

Vulnerabilities are handled as [SECURITY.md](SECURITY.md) says: reported
privately, answered within seven days, fixed in a release, and disclosed
with credit to the reporter.

## Continuity

The project is GPL-3.0-or-later, its whole history is public on GitHub,
and everything needed to build, test and release it is in the repository
(`tools/ci.sh`, the workflows, CONTRIBUTING.md), so anyone can carry it on
from a fork.
