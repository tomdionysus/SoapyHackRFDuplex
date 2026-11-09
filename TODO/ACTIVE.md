# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date, with the figures the
acceptance test produced.

## Resume here

Read `CLAUDE.md`, then this file, then `CLAUDE.local.md` (the two boards,
their serials, the bench). The code is described in
[docs/design.md](../docs/design.md); read "Components", "Time" and
"Reference switches" before touching `pair.cpp`.

Build and test:

```
cmake --preset default && cmake --build build -j8 && ctest --test-dir build
export SOAPY_SDR_PLUGIN_PATH=$PWD/build/module
./build/tools/hackrfduplex-check --clkout rx --tx-tone 100e3 --tx-gain 0 --seconds 5
```

The last line is the smoke test of the whole pair: it should report the
origin found at start, the tone at +0.0 Hz in every second, 0
underflows, and at most 1 overflow (the receive ring fills while the
second activation calibrates). `--timed-bursts 81920 --seconds 60`
checks the hardware time: every burst at +0 samples. The bench
is shielded and Tom has given a standing go-ahead for transmit tests for
this work (standing decisions, below); keep TX gain at 0 and the
amplifier off unless an item says otherwise, and leave `clkout=rx` set at
the end of a run so the boards stay on one reference.

Work the items in order. Item 7 measures the hardware time built in
items 4 to 6 (docs/design.md, Time); items 8 and 9 close the project
out. The trigger item waits on a jumper Tom has not fitted.

## Where things stand

- 2026-11-09: the 2023 code was rewritten to the 2026 conventions and
  built against SoapySDR 0.9.0, libhackrf 2026.01.3 and Catch2 3.16.1;
  51 test cases pass. Both boards run the 2026.01.3 release firmware.
- Verified on the bench: receive at 2, 8 and 20 Msps without an overflow;
  a tone in every sample format received at exactly its sent offset with
  the boards on one reference; a board whose reference changes at stream
  start is retuned and restarted, both ways on both boards
  (TODO/COMPLETED.md, docs/design.md "Measured").
- Also done on 2026-11-09 (TODO/COMPLETED.md): unknown settings ignored,
  the `lo_locked` sensor, rates below 2 Msps measured and advertised, and
  the hardware time: a common origin from the calibration burst, the
  hardware time API, timed transmit. 96 test cases pass.
- Not yet done: the transmit lead measured (item 7), the trigger origin
  on the bench, packaging. The upstream report is drafted in
  `TODO/upstream-reference-switch.md` for Tom to file if he wants.

## Standing decisions

- Scope (Tom, 2026-11-09): this project is a pair of HackRF Ones as one
  full-duplex SoapySDR device, and nothing else. Applications built on
  it, and changes to the HackRF firmware, CPLD or libhackrf, belong
  elsewhere. The module works with stock firmware and stock libhackrf.
- C++20 throughout, Catch2 v3, the hardware dialect of
  `/Users/tom/devroot/CLAUDE-STYLE-2026.md`. Tests come from the SoapySDR
  contract and this plan, written before the code; a test is never
  deleted.
- GPL-3.0-or-later. Tom's own project, not a fork; no code from other
  modules; only the SoapySDR conventions a HackRF user expects are kept.
- One board receives and one transmits, fixed for the life of the device.
  No role swapping, no single-board fallback.
- The RF amplifier is presented as 14 dB, as every SoapySDR HackRF driver
  has, although libhackrf documents about 11 dB.
- Sample rates: the module passes what it is given through without
  clamping, and advertises what was measured: 0.2 to 20 Msps for
  receive, 1.2 to 20 Msps for transmit (docs/design.md, Low sample
  rates).
- A requested bandwidth rounds down to a filter width, never up.
- Discovery never opens a board.
- Stream positions are sample counts from each stream's activation and
  already include samples dropped at the USB level (RxPath counts a
  dropped transfer, TxPath counts the zeros it sends). Receive timestamps
  are those positions at the sample rate.
- The common time origin is chosen by the `time_origin` setting, also a
  device argument: `burst` (default), `trigger`, `none` (Tom,
  2026-11-09). The default transmits a short calibration burst when the
  second stream starts. The pair never recalibrates on its own.
- Dependencies: SoapySDR, libhackrf, Catch2. Nothing else without a
  decision recorded here.
- Transmit tests: Tom's standing go-ahead for this work on the shielded
  bench ("you may test autonomously until completion", 2026-11-09). Ask
  again on any other bench, and before switching the amplifier on.
- Commits with no Claude text, and pushes to `origin develop`, at any
  time (Tom, 2026-11-09). One logical change per commit.

## Corrections to the time plan

The plan for items 4 to 7 was written before the code; where they
differed the code was right (docs/design.md, Time, holds the design as
built). For the record:

- The burst goes out at full scale on I and Q, not 0.7 on I, and the
  capture's mean is removed before correlating: at TX gain 0 on this
  bench the planned burst stood 12.4 dB over the median, under the
  receiver's DC offset; as built it stands 27 to 30 dB.
- Timed blocks are placed by the board's transfer thread, not by the
  writer writing zeros up to the target: silence the board inserts into
  an empty ring moves later samples, which only that thread knows.
- The origin is transmit position less receive position, so a transmit
  time T goes out at `rx_position(T) + origin`.
- The origin differs from start to start (it holds how far apart the
  host started the streams), so "ten starts give the same origin" was
  replaced by: an origin found at every start, and repeat calibrations
  within a start agreeing.
- The lead a timed write needs is about five USB transfers, not four:
  libhackrf keeps four transmit transfers ahead of the air, and a
  receive timestamp is up to one transfer old when read.

## Items

### 7. Transmit lead time

- [ ] With `--timed-bursts`, the smallest delay between a receive
  timestamp and a transmit time that gives no late reports over ten
  minutes, at 2, 8, 11.52, 15.36 and 20 Msps, at the default `buffers`
  and at 4. Acceptance: the table in `docs/design.md` "Measured", the
  `buffers` default revisited in its light, and the README's hardware
  time paragraph stating the lead a writer should allow.

### Trigger origin on the bench

- [ ] Waits on a jumper from the RX board's P28 pin 15 (trigger out) to
  the TX board's P28 pin 16 (trigger in), boards grounded together
  through the clock cable. The trigger origin is built and its unit
  tests pass against the fake board. Acceptance: with
  `--time-origin trigger`, `hackrfduplex-check --timed-bursts` lands
  every burst at one constant offset from the time asked (the trigger
  origin is 0 by construction, so the offset is the boards' pipeline
  difference), and without the jumper the start fails naming it.

### 9. Packaging and CI

- [ ] Debian packaging for the module and the tool; a build-and-test
  script that runs the format check, the tests and the sanitizer build.
  Acceptance: a `.deb` that installs on Debian trixie and
  `SoapySDRUtil --find` sees the module; the script passing on the
  workstation.
