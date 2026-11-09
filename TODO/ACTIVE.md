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
items 4 to 6 (docs/design.md, Time); item 9 waits on a trixie system,
the trigger item on a jumper. Items 10 to 22 take the module to the
1.0.0 release: "Release 1.0.0" below says what that means, and "Waits
on Tom" holds the decisions it needs first.

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

## Waits on Tom

Each with a recommendation; the items that depend on one say so.

1. **A trixie system** for the `.deb` (item 9) and the Linux soak
   (item 19): a host over ssh (recommended, and the soak wants real
   hardware), or a go-ahead for Docker Desktop, whose storage is outside
   the filesystem boundary.
2. **The transmit underrun limit** (item 13): libhackrf can make the
   transmitting board stop itself after a set run of underrun, so a
   transmitter cannot outlive a crashed or hung host. The module always
   feeds zeros, so the limit only trips when the host has truly stopped.
   Recommended: on by default at one transfer's worth, a setting to
   change it. The receive overrun limit stays off: it would stop a
   receiver whose reader was merely slow.
3. **Clock and time sources** (item 15): SoapySDR's `setClockSource`
   and `setTimeSource` are what applications call to share a reference
   and a time. Recommended: clock sources `internal` (each board on its
   own), `rx` and `tx` (that board drives the other, as `clkout` does
   now), and time sources `burst`, `trigger` and `none` (as
   `time_origin`), keeping the settings as aliases.
4. **No signal processing in the stream path**: software DC removal, I/Q
   balance and frequency correction would help HackRF users but are
   DSP, not a device. Recommended: out of scope for 1.0.0; the module
   answers `hasDCOffsetMode` and the like with false, as now.
5. **Hardware for coverage**: a board with id `HACKRF1_R9` (the other
   reference-switch path, never run), the trigger jumper, and a powered
   hub or root ports for 20 Msps both ways (item 18). Without them
   1.0.0 ships with those paths named as untested.
6. **The version**: 1.0.0, with the SoapySDR surface and the settings
   then treated as stable. Recommended.

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
- Done 2026-11-09: `tools/ci.sh` passes on the workstation (43 files
  formatted, licence lines, 96 test cases in the release and the
  sanitizer builds); `debian/` written for two packages; every source
  compiles under GCC 15 with `-Werror` (one shadowing fixed).
- Waits on Tom: a trixie system to build and install the `.deb` on.
  Docker Desktop is on the workstation, but its images and containers
  live under `~/Library`, outside the filesystem boundary, so it was not
  used without asking. A trixie host reachable over ssh, or a go-ahead
  for Docker, finishes it.

## Release 1.0.0

What "elite" means for this module, and the gate for the tag:

- **The contract, all of it.** Every SoapySDR call behaves as the
  contract says, per direction. What the device cannot do it refuses
  explicitly (`SOAPY_SDR_NOT_SUPPORTED`, or a throw naming why), and
  never silently ignores. A flag, argument or setting it accepts it
  honours.
- **Proven.** Every behaviour has a test against the fakes, written
  before its code. Every hardware claim in the docs is a figure from
  `hackrfduplex-check`, and one command certifies a pair.
- **Safe.** The transmitter cannot outlive its host. It transmits only
  what was written, plus the documented calibration burst, which
  `time_origin=none` turns off.
- **Honest under fault.** Lost samples, a lost board, a lost reference or
  a lost origin each reach the application as a stream status within a
  bounded time, never as silently wrong data or timestamps.
- **Fast and steady.** Full duplex at 20 Msps on a proper USB topology,
  measured CPU use, and a 24-hour soak with the origin held throughout.
- **Shipped.** A `.deb` that installs on trixie, docs a newcomer can
  follow, and tag `1.0.0` on `main`.

Out of scope, as the standing decisions say: firmware, CPLD and
libhackrf changes (so the transmit lead stays about five transfers),
and applications built on the pair. Items 10 to 22, in order:

### 10. Fix: calibration restores a stale transmit frequency

- [ ] `calibrate_locked` notes the transmit frequency before retuning for
  the burst and restores that value afterwards. If the application tunes
  the transmitter while calibration runs, the board ends up on the old
  frequency while `getFrequency` reports the new one. Restore the
  frequency the pair holds at the moment of restoring, under the side's
  lock. Acceptance: a test that tunes TX from the board hook during the
  burst and finds board and pair agreeing afterwards.

### 11. Fix: lost samples are reported whether or not there is an origin

- [ ] A board shortfall loses samples the stream positions cannot see,
  so receive timestamps after it are wrong even with no time origin.
  Today shortfalls are only polled while an origin holds. Poll them
  whenever a stream is active: a receive shortfall reports
  `SOAPY_SDR_OVERFLOW` once with `SOAPY_SDR_END_ABRUPT` and a log line
  saying the timestamps have shifted by an unknown amount; a transmit
  shortfall reports `SOAPY_SDR_UNDERFLOW` once. Losing the origin
  follows as now. Acceptance: fake-board tests for each case, with and
  without an origin.

### 12. Refuse what is not supported

- [ ] Audit every SoapySDR entry point for arguments accepted and
  ignored. Known: `activateStream` ignores `SOAPY_SDR_HAS_TIME` and
  `numElems`, and `deactivateStream` ignores its time. Each is either
  implemented (item 14) or refused with `SOAPY_SDR_NOT_SUPPORTED`, and
  getters for absent features answer as the SoapySDR defaults do.
  Acceptance: one test per entry point and flag, a table of the
  SoapySDR API against the module in `docs/design.md`, and
  `SoapySDRUtil --probe` unchanged.

### 13. Safety: the transmitter cannot outlive its host

- [ ] Waits on decision 2. Set the transmitting board's underrun limit at
  each start (`hackrf_set_tx_underrun_limit`, through a new `Board`
  call), with a `tx_underrun_limit` setting. Detect the board having
  stopped itself (it stops streaming) and report it as a stream error.
  Add a transmit audit test: over a run of writes, timed writes, bursts,
  gaps, late drops, calibration and deactivation, the fake transmitter
  receives exactly the written samples, the calibration burst and zeros,
  nothing else. Acceptance: those tests, and on the bench a check run
  killed with SIGKILL mid-transmit leaves the TX board idle within the
  limit (`hackrf_debug` M0 state, or `hackrf_info` showing no
  streaming).

### 14. Timed receive: start at a time, finite bursts

- [ ] `activateStream` on the receive stream with `SOAPY_SDR_HAS_TIME`
  delivers from that time on (earlier samples discarded, the first read
  stamped exactly at it), and with `SOAPY_SDR_END_BURST` and `numElems`
  delivers that many samples and ends with `SOAPY_SDR_END_BURST` on
  the last read. A time already passed returns `SOAPY_SDR_TIME_ERROR`.
  Acceptance: fake-board tests; on the bench, a timed receive window
  that starts exactly where a timed transmit burst lands, through a new
  `hackrfduplex-check --timed-window` mode.

### 15. Clock and time sources through the SoapySDR API

- [ ] Waits on decision 3. `listClockSources`, `setClockSource`,
  `getClockSource`, `listTimeSources`, `setTimeSource` and
  `getTimeSource` mapped onto `clkout` and `time_origin`, which stay as
  settings. Before calibrating, check that the boards share a reference
  (the fed board reports CLKIN), and refuse with a log line naming the
  `clkout` setting rather than burning a burst that cannot be heard:
  free-running, the crystals turn the chips over within the burst.
  Acceptance: tests for the mapping and the refusal; the probe listing
  them.

### 16. Device-level sensors

- [ ] Beside the per-channel `lo_locked`: `ref_locked` (the boards share
  one reference: the fed board sees its CLKIN and the other drives
  CLKOUT) and `time_locked` (an origin holds), both booleans, read
  without disturbing the streams. Clock-chip registers read as zero
  while a board streams, so `ref_locked` comes from CLKIN status, not
  the registers. Acceptance: fake-board tests for both; on the bench,
  `ref_locked` false with `clkout=off` and true with `clkout=rx`, and
  `time_locked` following the streams.

### 17. Bounded activation and an early end to calibration

- [ ] Calibration listens for its worst case (about 27 transfers, 0.45 s
  at 8 Msps, 1.8 s at 1.92 Msps) inside `activateStream`, and a reader
  not yet running overflows meanwhile. Correlate transfer by transfer
  as the capture fills and stop at a peak that clears the threshold
  with the burst's full length behind it, so activation takes about as
  long as the burst takes to come round. Acceptance: fake-board tests
  that it stops early and still finds the same origin; on the bench the
  activation time measured at 2, 8 and 20 Msps before and after, and
  the origin unchanged across ten calibrations.

### 18. Performance

- [ ] Measure CPU per direction and format at 2, 8 and 20 Msps (the
  check tool reporting process CPU time). Make the conversions
  vectorise by hand-written loops the compiler can auto-vectorise (no
  library). Add SoapySDR's direct buffer access (`acquireReadBuffer`
  and friends) for CS8, so a native-format reader copies nothing.
  Acceptance: before and after table in `docs/design.md`; direct access
  tests against the fake; full duplex at 20 Msps both ways for ten
  minutes with no overflow, underflow or shortfall on a proper USB
  topology (waits on decision 5's hub), or the topology's limit
  recorded if it cannot.

### 19. Quality gates

- [ ] Clang thread-safety annotations on every guarded member, with
  `-Wthread-safety` on, and the lock order (control, then side, then
  time) written beside the locks. A `tsan` preset. clang-tidy with a
  checked-in configuration, clean. A coverage report with the core at
  or near complete line coverage, gaps listed and justified. `tools/ci.sh`
  gains the tsan run and the coverage summary. A full review of the
  diff since 0.1.0 (`/code-review`), findings fixed or recorded.
  Acceptance: `tools/ci.sh` passing with all of it, and the coverage
  figure in COMPLETED.md.

### 20. Interoperability

- [ ] Run the pair from the applications people use, and record what
  works in `docs/compatibility.md`: `SoapySDRUtil --probe` and `--rate`;
  the SoapySDR Python bindings (a timed loopback script under `tools/`);
  GNU Radio's Soapy source and sink in one full-duplex flowgraph;
  SoapyRemote serving the pair to another process; one receive-only
  application (SDR++ or CubicSDR). Each with the version used, what was
  checked, and any quirk. Acceptance: the matrix filled, every failure
  either fixed in the module or explained as the application's.

### 21. Certification and soak

- [ ] `hackrfduplex-check --acceptance` runs the whole hardware battery
  and prints pass or fail per line against the figures in
  `docs/design.md`: receive at 2, 8 and 20 Msps; a tone in each format;
  a reference switch each way; the origin repeatable; timed bursts at
  +0; low rates; `lo_locked`, `ref_locked`, `time_locked`. It transmits
  only with `--transmit`. Then a 24-hour soak on Linux: full duplex at
  8 Msps with timed bursts throughout, the origin holding (no shortfall)
  or each loss reported and recovered with `calibrate`, memory flat.
  Acceptance: the battery passing on this pair, and the soak's figures
  in COMPLETED.md.

### 22. Documentation and the release

- [ ] `docs/usage.md`, a guide for an application writer: setting up the
  pair (clock cable, USB topology), what the time origin is and where
  the calibration burst transmits (the receive frequency, at the
  transmit gain set), the transmit lead to allow, the stream statuses
  and what each means, and troubleshooting (no origin found, reference
  switches, shortfalls, low rates). A man page for `hackrfduplex-check`
  installed by CMake and the package. README brought to its final form,
  the status table all ticks or named exceptions. Then: version 1.0.0
  in `CMakeLists.txt` and `debian/changelog`, `tools/ci.sh` passing,
  `develop` merged to `main`, an annotated tag `1.0.0` on `main`, and
  `main`, `develop` and the tag pushed to `origin`. Acceptance: a
  newcomer's path from the README to a timed loopback works as written,
  checked by following it from a clean clone; `module_version` reads
  1.0.0 in `SoapySDRUtil --probe` on the built package.

