# hackrfduplex

[![CI](https://github.com/tomdionysus/SoapyHackRFDuplex/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/SoapyHackRFDuplex/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/SoapyHackRFDuplex/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/SoapyHackRFDuplex)
[![CodeQL](https://github.com/tomdionysus/SoapyHackRFDuplex/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/SoapyHackRFDuplex/actions/workflows/codeql.yml)

Version 0.2.0

A [SoapySDR](https://github.com/pothosware/SoapySDR/wiki) module that
makes two [HackRF One](https://greatscottgadgets.com/hackrf/one/) boards
one full-duplex radio. One board receives and the other transmits, and
an application sees a single device, `driver=hackrfduplex`, with an RX
and a TX channel that run at the same time, each tuned on its own and
sharing one clock and one timeline.

- `soapysdr0.8-module-hackrfduplex`, the SoapySDR module
- `hackrfduplex-check`, which exercises the pair as an application would
  and reports what it finds
- `hackrfduplex-ft8`, an example FT8 decoder through SoapySDR

It works with stock HackRF firmware and stock libhackrf.

- Every SoapySDR call behaves as the contract says, per direction; what
  the device cannot do it refuses, never silently ignores.
- Timed transmit to the sample: in a four-hour soak on the real pair,
  over a million timed bursts were each received at the sample asked.
- Unit tests against simulated boards on every push, with address,
  undefined-behaviour and thread sanitizers, Clang's thread-safety
  analysis, clang-tidy and CodeQL.
- A hardware acceptance battery certifies a pair in one command.
- Runs with GNU Radio, SoapyRemote, rtl_433 and SoapySDR's Python
  bindings.

## Install

On Debian trixie, build and install the packages from source:

```
sudo apt install build-essential debhelper cmake pkgconf libhackrf-dev libsoapysdr-dev catch2
git clone https://github.com/tomdionysus/SoapyHackRFDuplex.git
cd SoapyHackRFDuplex
dpkg-buildpackage -us -uc -b
sudo apt install ../soapysdr0.8-module-hackrfduplex_*.deb ../hackrfduplex-tools_*.deb
```

Other systems, and the build options: [docs/building.md](docs/building.md).

## Getting started

- [Quickstart](docs/quickstart.md): connect, install, find and check the
  pair
- [Using the pair](docs/usage.md): the shared time, transmitting at a
  time, stream statuses, troubleshooting
- [Reference](docs/reference.md): every argument, setting, range and
  sensor

All documentation: [docs/index.md](docs/index.md). What works and what
is untested: [docs/status.md](docs/status.md).

## Roadmap

- The trigger time origin, which starts both boards on one edge through
  a jumper instead of a calibration burst, is built but has not been on
  the bench yet.
- If someone has a genuine r9 or later HackRF One, I'd like to hear how
  reference switching goes on it: those boards select their reference
  differently, and that path has not met one.
- Full duplex at 20 Msps needs a host whose USB carries it; a Raspberry
  Pi 5 tops out at 16.

Applications built on the pair, and changes to the HackRF firmware or
libhackrf, are out of scope.

## AI

This project was developed with the use of agentic engineering,
specifically Claude. If that's a problem for you,
[SoapyHackRF](https://github.com/pothosware/SoapyHackRF) drives a single
HackRF.

## Licensing

Licensed under the GNU General Public License, version 3 or (at your
option) any later version; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Third-party works and their licences are listed in
[docs/licensing.md](docs/licensing.md).
