# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date.

## Where things stand

- 2026-11-09: the 2023 fork was rewritten to the 2026 conventions and
  built against SoapySDR 0.9.0, libhackrf 2026.01.3 and Catch2 3.16.1:
  47 test cases pass, `SoapySDRUtil --find` lists both orderings of the
  attached pair, `--probe` opens it, and `hackrfduplex-check` receives at
  2, 8 and 20 Msps for 10 s with 0 overflows and the rate within 0.1% of
  nominal ([docs/design.md](../docs/design.md), Measured).
- 2026-11-09, later: the 2023 files are gone and `cmake --preset default`
  builds and passes the tests in a clean `build/`. Both boards were
  flashed to the 2026.01.3 release firmware (USB API 1.10) and the pair
  probes with both reporting their TCXO as an external clock.
- Not yet done on hardware: transmitting (item 1, needs Tom's go-ahead).
- Two boards are attached to the workstation (`CLAUDE.local.md`).

## Standing decisions

- C++20 throughout, Catch2 v3, the hardware dialect of
  `/Users/tom/devroot/CLAUDE-STYLE-2026.md`.
- GPL-3.0-or-later, with SoapyHackRF's MIT notice kept in `NOTICE`.
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

### 1. Verify transmit on hardware

- [ ] With Tom's go-ahead: `hackrfduplex-check --tx-tone 100e3 --tx-gain 0
  --seconds 10`, amplifier off, stub antennas at 20 cm. Acceptance: the
  tone is at least 30 dB above the mean level at the receiver, 0
  underflows, 0 overflows, the rate within 1% of nominal; stopping the
  tool leaves both boards idle (`hackrf_info` opens them). Then the same
  with CS16 and CS8 formats through a short client, or the tool extended
  with `--format`.

### 2. Shared reference clock

- [ ] Both boards carry their own 10 MHz TCXO on CLKIN, so each is stable
  but they are not locked to each other. First measure what that costs:
  the tone's frequency offset and its drift at the receiver over a
  minute, from item 1's setup. Then decide whether to lock them (one
  board's TCXO removed, its CLKIN fed from the other's CLKOUT, with
  `hackrf_set_clkout_enable` as a `clkout` setting) or whether two TCXOs
  are close enough for the cell. Acceptance: the measured offset and
  drift recorded in `docs/design.md`, and the decision recorded here.

### 3. Synchronised start

- [ ] Hardware sync mode on both boards, trigger pins wired, so RX and TX
  streams begin on one edge; the pair records the common origin.
  Acceptance: a tone burst written at TX position P is received at a
  fixed RX position P plus a constant, repeatable to a few samples across
  ten starts. Check first with `hackrf_transfer -H` whether TX honours
  the trigger on this firmware.

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
