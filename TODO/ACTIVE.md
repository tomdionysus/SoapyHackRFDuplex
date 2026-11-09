# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- 2026-11-09: the 2023 code was rewritten to the 2026 conventions and
  built against SoapySDR 0.9.0, libhackrf 2026.01.3 and Catch2 3.16.1.
  Both boards run the 2026.01.3 release firmware.
- Full duplex verified on the bench: receive at 2, 8 and 20 Msps without
  an overflow; a tone in every sample format received at exactly its sent
  offset with the boards on one reference; a board whose reference changes
  at stream start is retuned and restarted, verified both ways on both
  boards (TODO/COMPLETED.md, docs/design.md).
- Two boards are attached to the workstation (`CLAUDE.local.md`).

## Standing decisions

- Scope (Tom, 2026-11-09): this project is a pair of HackRF Ones as one
  full-duplex SoapySDR device, and nothing else. Applications built on
  it, such as an LTE cell, and changes to the HackRF firmware, CPLD or
  libhackrf belong elsewhere. The module works with stock firmware and
  stock libhackrf.
- C++20 throughout, Catch2 v3, the hardware dialect of
  `/Users/tom/devroot/CLAUDE-STYLE-2026.md`.
- GPL-3.0-or-later. This is Tom's own project, not a fork; it takes no
  code from other modules and keeps only the SoapySDR conventions a
  HackRF user expects.
- One board receives and one transmits, fixed for the life of the device.
  No role swapping, no single-board fallback.
- The RF amplifier is presented as 14 dB, as every SoapySDR HackRF driver
  has, although libhackrf documents about 11 dB.
- Sample rates 2 to 20 Msps as libhackrf documents; the module passes
  what it is given to libhackrf without clamping.
- A requested bandwidth rounds down to a filter width, never up.
- Discovery never opens a board.
- The receive stream has a sample-count time from activation; the
  transmit stream has none unless item 3 is taken up.
- Dependencies: SoapySDR, libhackrf, Catch2. Nothing else without a
  recorded decision here.
- Transmit tests: Tom's standing go-ahead for this work, on the shielded
  bench (2026-11-09). Ask again on any other bench.

## Items

### 1. Report the firmware's reference-switch behaviour upstream

- [ ] The firmware switches clock source after tuning and does not
  retune, and caches the applied frequency so a retune to the same value
  is skipped ([docs/design.md](../docs/design.md), Reference switches).
  The module works around both. Draft an issue for Great Scott Gadgets
  with the reproduction, for Tom to file if he wants. Acceptance: the
  draft in `TODO/`, nothing posted.

### 2. Packaging and CI

- [ ] Debian packaging for the module and the tool; a build-and-test
  script that runs the format check, the tests and the sanitizer build.
  Acceptance: a `.deb` that installs on Debian trixie and
  `SoapySDRUtil --find` sees the module.

### 3. Hardware time (optional, Tom to decide)

- [ ] SoapySDR devices may offer a hardware time: `hasHardwareTime`,
  receive timestamps that share an origin with timed transmit. For the
  pair that means hardware sync mode on both boards with their trigger
  pins wired, so both streams start on one edge, then `writeStream` with
  `SOAPY_SDR_HAS_TIME` placing samples at a stream position. It is a
  device feature, within scope, but needs a wire between the boards and
  is only worth doing if an application wants it. Acceptance, if taken
  up: a burst written for time T is received at T plus a constant,
  repeatable to a few samples across ten starts.
