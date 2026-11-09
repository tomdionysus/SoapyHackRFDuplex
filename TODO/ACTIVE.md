# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- 2026-11-09: the 2023 code was rewritten to the 2026 conventions and
  built against SoapySDR 0.9.0, libhackrf 2026.01.3 and Catch2 3.16.1:
  47 test cases pass, `SoapySDRUtil --find` lists both orderings of the
  attached pair, `--probe` opens it, and `hackrfduplex-check` receives at
  2, 8 and 20 Msps for 10 s with 0 overflows and the rate within 0.1% of
  nominal ([docs/design.md](../docs/design.md), Measured).
- 2026-11-09, later: the 2023 files are gone and `cmake --preset default`
  builds and passes the tests in a clean `build/`. Both boards were
  flashed to the 2026.01.3 release firmware (USB API 1.10) and the pair
  probes with both reporting their TCXO as an external clock.
- Full duplex verified on the air the same day with the boards on one
  reference: a tone in every sample format sent at +100 kHz arrived at
  exactly +100 kHz, 0 overflows, 0 underflows (TODO/COMPLETED.md).
- A board whose reference changes at stream start is retuned and
  restarted, so `clkout` may be switched freely between runs; verified
  in both directions on both boards.
- Tom gave standing permission on 2026-11-09 to run transmit tests
  autonomously until this work is complete; the bench is shielded.
- Two boards are attached to the workstation (`CLAUDE.local.md`).

## Standing decisions

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
  transmit stream has none until the start of both streams can be
  aligned (item 3).
- Dependencies: SoapySDR, libhackrf, Catch2. Nothing else without a
  recorded decision here.
- No transmit test without Tom's go-ahead for that test, at the time.

## Items

### 1. Report the firmware's reference-switch behaviour upstream

- [ ] The firmware switches clock source after tuning and does not
  retune, and caches the applied frequency so a retune to the same value
  is skipped ([docs/design.md](../docs/design.md), Reference switches).
  The module works around both. Draft an issue for Great Scott Gadgets
  with the reproduction, for Tom to file if he wants. Acceptance: the
  draft in `TODO/`, nothing posted.

### 3. Synchronised start

- [ ] Hardware sync mode on both boards, trigger pins wired, so RX and TX
  streams begin on one edge; the pair records the common origin.
  Acceptance: a tone burst written at TX position P is received at a
  fixed RX position P plus a constant, repeatable to a few samples across
  ten starts. The CPLD source (`firmware/cpld/sgpio_if/top.vhd` in the
  hackrf tree) gates sample capture on the sync input for both
  directions, so TX should honour the trigger; confirm on the bench with
  `hackrf_transfer -H` before building on it.

### 4. Transmit timestamps

- [ ] `writeStream` with `SOAPY_SDR_HAS_TIME` schedules samples at a
  stream position derived from the common origin, zero-filling the gap.
  Acceptance: a burst asked for at time T arrives at the receiver at T
  plus the constant measured in item 3.

### 5. Latency

- [ ] Measure the loop from a change at the transmitter to its arrival at
  the reader at 8 and 15.36 Msps with stock libhackrf; then a libhackrf
  built with 16 KiB transfers. Record both in `docs/design.md`.
  Acceptance: the measured figures, and a decision on the transfer size
  an LTE cell needs.

### 6. An LTE cell

- [ ] srsRAN eNB on the pair at 10 MHz bandwidth with a UE attaching.
  Depends on items 2 to 5. Acceptance: a UE completes attach and passes
  traffic. Recorded with the MCS reached.

### 7. Packaging and CI

- [ ] Debian packaging for the module and the tool; a build-and-test
  script that runs the format check, the tests and the sanitizer build.
  Acceptance: a `.deb` that installs on Debian trixie and
  `SoapySDRUtil --find` sees the module.
