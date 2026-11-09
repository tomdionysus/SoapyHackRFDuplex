# Active work

Open work only, ordered by dependency: take the first unchecked item whose
dependencies are done. Each item names its acceptance test. A finished
item is removed from here and recorded at the top of
[TODO/COMPLETED.md](COMPLETED.md) with its date and the figures its
acceptance test produced.

## Resume here

Read `CLAUDE.md`, then this file, then `CLAUDE.local.md` (the build host,
the boards, the bench). The code is described in
[docs/design.md](../docs/design.md); read "Components", "Threads", "Time"
and "Reference switches" before touching `pair.cpp` or `tx_path.cpp`.

Everything builds and runs on the target, the Debian trixie host with
both boards attached, over ssh. Edit here; build and test there:

```
export HACKRFDUPLEX_TARGET=<from CLAUDE.local.md>
tools/ci-pi.sh                              # format, licences, release and sanitizer builds and tests
tools/ci-pi.sh -- tools/ci.sh --quick       # release build and tests only
tools/ci-pi.sh -- bash -c 'SOAPY_SDR_PLUGIN_PATH=$PWD/build/module ./build/tools/hackrfduplex-check --clkout rx --seconds 5'
```

The last line is the receive smoke test: 99.9% of nominal, 0 overflows,
both boards `clock: external`. With transmit allowed, add `--tx-tone
100e3 --tx-gain 0`: the origin found at start, the tone at +0.0 Hz every
second, 0 underflows, at most 1 overflow (the receive ring fills while
the second activation calibrates); and `--timed-bursts 81920 --seconds
60`: every burst at +0 samples. Leave `clkout=rx` set at the end of a
run. The workstation can still build and run the unit tests for a quick
test-first loop; the target's result is the one that counts.

Work the items in order. Item 1 brings the pair up on the target, item 2
finishes measuring the hardware time, items 3 to 16 take the module to
the 1.0.0 release ("Release 1.0.0" says what that means). "Waits on Tom"
lists what blocks which item.

## Where things stand

- 2026-11-09: the module does what it set out to do. Discovery, the
  pair, both streams in four formats, tuning, filters, gains, bias tee,
  the shared reference (`clkout`) with reference switches handled, the
  `lo_locked` sensor, unknown settings tolerated, rates from 0.2 Msps
  (receive) and 1.2 Msps (transmit), and the hardware time: a common
  origin from a calibration burst (or a trigger edge), the hardware time
  API, timed transmit placed to the sample. 96 test cases pass, in the
  release and sanitizer builds, on the workstation and on the target.
- Verified on the workstation's bench (docs/design.md, Measured):
  receive at 2, 8 and 20 Msps; a tone in every format at its sent offset;
  reference switches both ways; low rates; the origin found at 20 starts
  of 20 and repeatable to 0 samples; 3658 timed bursts at +0 samples.
- The same day the work moved to the target, a Raspberry Pi 5 on trixie
  with each board on its own root port. There: `tools/ci.sh` passes; the
  `.deb` builds, installs and is found by `SoapySDRUtil --find`; receive
  at 8 Msps is clean; 20 Msps through the check tool is CPU-bound (81%),
  while USB alone carries the full rate.
- The upstream report on the firmware's reference switch is drafted in
  `TODO/upstream-reference-switch.md`, for Tom to file if he wants.

## Waits on Tom

Each with a recommendation; the items that depend on one say so.

1. **Transmitting from the target.** The boards moved to the Pi; is it
   the same shielded bench, so the standing go-ahead carries over? Until
   then only receive work runs there (items 1 and 2 have transmit parts).
2. **The Pi's power supply.** `vcgencmd get_throttled` read `0x50000`:
   under-voltage has happened since boot. Unless it is the 5 A supply, a
   Pi 5 limits USB to 600 mA in all, which two HackRFs can exceed, and a
   sagging supply looks exactly like a board shortfall. Recommended: the
   5 A supply, or the boards on a powered hub on the root ports.
3. **The transmit underrun limit** (item 6): libhackrf can make the
   transmitting board stop itself after a run of underrun, so a
   transmitter cannot outlive a crashed or hung host. The module always
   feeds zeros, so it trips only when the host has truly stopped.
   Recommended: on by default at one transfer's worth, with a setting.
   The receive overrun limit stays off: it would stop a receiver whose
   reader was merely slow.
4. **Clock and time sources** (item 8): SoapySDR's `setClockSource` and
   `setTimeSource` are what applications call to share a reference and
   a time. Recommended: clock sources `internal`, `rx` and `tx` (as
   `clkout`), time sources `burst`, `trigger` and `none` (as
   `time_origin`), the settings kept as aliases.
5. **No signal processing in the stream path**: software DC removal, I/Q
   balance and frequency correction are DSP, not a device. Recommended:
   out of scope for 1.0.0; the module answers `hasDCOffsetMode` and the
   like with false, as now.
6. **Hardware for coverage**: the trigger jumper (item 14), and a board
   with id `HACKRF1_R9` for the other reference-switch path, never run.
   Without them 1.0.0 ships with those paths named as untested.
7. **The version**: 1.0.0, the SoapySDR surface and the settings then
   held stable. Recommended.

## Standing decisions

- Scope (Tom, 2026-11-09): this project is a pair of HackRF Ones as one
  full-duplex SoapySDR device, and nothing else. Applications built on
  it, and changes to the HackRF firmware, CPLD or libhackrf, belong
  elsewhere. The module works with stock firmware and stock libhackrf,
  so the transmit lead stays about five USB transfers.
- The build of record is on the target over ssh (`tools/ci-pi.sh`),
  Debian trixie with the distribution's libhackrf, SoapySDR and Catch2
  (Tom, 2026-11-09).
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
  include samples dropped at the USB level (RxPath counts a dropped
  transfer, TxPath counts the silence it sends). Receive timestamps are
  those positions, plus the offset `setHardwareTime` sets, at the
  receive rate.
- The common time origin is chosen by the `time_origin` setting, also a
  device argument: `burst` (default), `trigger`, `none` (Tom,
  2026-11-09). The default transmits a short calibration burst on the
  receive frequency when the second stream starts. The pair never
  recalibrates on its own.
- Dependencies: SoapySDR, libhackrf, Catch2. Nothing else without a
  decision recorded here.
- Transmit tests: Tom's go-ahead for this work on the shielded bench
  ("you may test autonomously until completion", 2026-11-09); whether
  it covers the target's bench waits on Tom (above). Ask before
  switching the amplifier on. The Radioberry on the target may be used
  for receive tests only (Tom, 2026-11-09).
- Commits with no Claude text, and pushes to `origin develop`, at any
  time (Tom, 2026-11-09). One logical change per commit.

## Release 1.0.0

What "elite" means for this module, and the gate for the tag:

- **The contract, all of it.** Every SoapySDR call behaves as the
  contract says, per direction. What the device cannot do it refuses
  explicitly (`SOAPY_SDR_NOT_SUPPORTED`, or a throw naming why), and
  never silently ignores. A flag, argument or setting it accepts it
  honours.
- **Proven.** Every behaviour has a test against the fakes, written
  before its code. Every hardware claim in the docs is a figure from
  `hackrfduplex-check` on the target, and one command certifies a pair.
- **Safe.** The transmitter cannot outlive its host. It transmits only
  what was written, plus the documented calibration burst, which
  `time_origin=none` turns off.
- **Honest under fault.** Lost samples, a lost board, a lost reference or
  a lost origin each reach the application as a stream status within a
  bounded time, never as silently wrong data or timestamps.
- **Fast and steady.** Full duplex at 20 Msps, measured CPU use, and a
  24-hour soak with the origin held throughout.
- **Shipped.** A `.deb` that installs on trixie, docs a newcomer can
  follow, and tag `1.0.0` on `main`.

## Items

### 1. Bring the pair up on the target

- [ ] Receive now; transmit once Tom confirms the bench (waits on 1).
  Receive: `hackrfduplex-check` at 2 and 8 Msps clean for 10 s each,
  both boards `clock: external` with `clkout=rx`, a reference switch
  handled each way (CLKOUT off, start, CLKOUT on, start: the
  `lo_locked` sensor and the log show it). Transmit: the smoke test
  above (origin found, tone at +0.0 Hz, 0 underflows) and `--timed-bursts
  81920 --seconds 60` with every burst at +0. Optionally, with the
  Radioberry receiving over HPSDR (receive only), the HackRF tone seen by
  a receiver that is not the other HackRF. Acceptance: the figures in
  COMPLETED.md, with the host named, and `docs/design.md` "Measured"
  saying which figures came from which host class (an Intel Mac on
  shared hubs, a Raspberry Pi 5 on root ports).

### 2. Transmit lead time

- [ ] With `--timed-bursts`, the smallest delay from a receive timestamp
  to a transmit time that gives no late reports over ten minutes, at 2,
  8, 11.52, 15.36 and 20 Msps, at the default `buffers` and at 4, on the
  target (waits on 1 for transmit). Method used so far: step the delay
  up in quarter transfers (131072 samples) from 4.5 with 30 s runs, then
  confirm the first clean one for 600 s, stepping on if it fails; a
  script to do it is a candidate for `tools/` or a check-tool mode.
  Figures so far, from the workstation before the boards moved: 2 Msps
  clean at 5.25 transfers (default buffers) and 5.5 (buffers 4, where
  5.25 had 4 late writes in ten minutes); 8 Msps clean at 5.0 for 30 s.
  Every write is late or none is, and the threshold moves by up to a
  transfer between starts, because it depends on how the two boards'
  transfer boundaries fall: so measure several starts per point and
  state the worst. Acceptance: the table in `docs/design.md` "Measured",
  the `buffers` default revisited in its light, and the README's
  hardware time paragraph stating the lead a writer should allow.

### 3. Fix: calibration restores a stale transmit frequency

- [ ] `calibrate_locked` notes the transmit frequency before retuning for
  the burst and restores that value afterwards. If the application tunes
  the transmitter while calibration runs, the board ends up on the old
  frequency while `getFrequency` reports the new one. Restore the
  frequency the pair holds at the moment of restoring, under the side's
  lock. Acceptance: a test that tunes TX from the board hook during the
  burst and finds board and pair agreeing afterwards.

### 4. Fix: lost samples and lost boards are reported

- [ ] A board shortfall loses samples the stream positions cannot see,
  so receive timestamps after it are wrong even with no time origin;
  today shortfalls are only polled while an origin holds. Poll them
  whenever a stream is active: a receive shortfall reports
  `SOAPY_SDR_OVERFLOW` once with `SOAPY_SDR_END_ABRUPT` and a log line
  saying the timestamps have shifted by an unknown amount; a transmit
  shortfall reports `SOAPY_SDR_UNDERFLOW` once; losing the origin
  follows as now. A board unplugged mid-stream was seen on 2026-11-09 to
  produce only the "origin lost" warning and then read timeouts until
  the caller gave up: a board that has stopped streaming (`Board::streaming`
  false, or a libhackrf error) must turn reads and writes into
  `SOAPY_SDR_STREAM_ERROR` within a bounded time. Acceptance: fake-board
  tests for each case, with and without an origin, and on the target a
  board unplugged mid-run giving a stream error within a second.

### 5. Refuse what is not supported

- [ ] Audit every SoapySDR entry point for arguments accepted and
  ignored. Known: `activateStream` ignores `SOAPY_SDR_HAS_TIME` and
  `numElems`, and `deactivateStream` ignores its time. Each is either
  implemented (item 7) or refused with `SOAPY_SDR_NOT_SUPPORTED`, and
  getters for absent features answer as the SoapySDR defaults do.
  Acceptance: one test per entry point and flag, a table of the
  SoapySDR API against the module in `docs/design.md`, and
  `SoapySDRUtil --probe` unchanged.

### 6. Safety: the transmitter cannot outlive its host

- [ ] Waits on decision 3. Set the transmitting board's underrun limit at
  each start (`hackrf_set_tx_underrun_limit`, through a new `Board`
  call), with a `tx_underrun_limit` setting; detect the board having
  stopped itself and report it as a stream error (item 4's path). Add a
  transmit audit test: over a run of writes, timed writes, bursts, gaps,
  late drops, calibration and deactivation, the fake transmitter is
  handed exactly the written samples, the calibration burst and zeros,
  nothing else. Acceptance: those tests, and on the target a check run
  killed with SIGKILL mid-transmit leaving the TX board idle within the
  limit (waits on 1 for transmit).

### 7. Timed receive: start at a time, finite bursts

- [ ] `activateStream` on the receive stream with `SOAPY_SDR_HAS_TIME`
  delivers from that time on (earlier samples discarded, the first read
  stamped exactly at it), and with `SOAPY_SDR_END_BURST` and `numElems`
  delivers that many samples, the last read flagged `SOAPY_SDR_END_BURST`.
  A time already passed returns `SOAPY_SDR_TIME_ERROR`. Acceptance:
  fake-board tests; on the target, a timed receive window that starts
  exactly where a timed transmit burst lands, through a new
  `hackrfduplex-check --timed-window` mode.

### 8. Clock and time sources through the SoapySDR API

- [ ] Waits on decision 4. `listClockSources`, `setClockSource`,
  `getClockSource`, `listTimeSources`, `setTimeSource` and
  `getTimeSource` mapped onto `clkout` and `time_origin`, which stay as
  settings. Before calibrating, check that the boards share a reference
  (the fed board reports CLKIN) and refuse with a log line naming the
  `clkout` setting rather than send a burst that cannot be heard:
  free-running, the crystals turn the chips over within the burst.
  Acceptance: tests for the mapping and the refusal; the probe listing
  them.

### 9. Device-level sensors

- [ ] Beside the per-channel `lo_locked`: `ref_locked` (the boards share
  one reference: the fed board sees its CLKIN and the other drives
  CLKOUT) and `time_locked` (an origin holds), both booleans, read
  without disturbing the streams. Clock-chip registers read as zero
  while a board streams, so `ref_locked` comes from CLKIN status, not
  the registers. Acceptance: fake-board tests for both; on the target,
  `ref_locked` false with `clkout=off` and true with `clkout=rx`, and
  `time_locked` following the streams.

### 10. Bounded activation and an early end to calibration

- [ ] Calibration listens for its worst case (about 27 transfers, 0.45 s
  at 8 Msps, 1.8 s at 1.92 Msps) inside `activateStream`, and a reader
  not yet running overflows meanwhile. Correlate transfer by transfer
  as the capture fills and stop at a peak that clears the threshold
  with the burst's full length behind it. Acceptance: fake-board tests
  that it stops early and finds the same origin; on the target the
  activation time at 2, 8 and 20 Msps before and after, and the origin
  unchanged across ten calibrations.

### 11. Performance

- [ ] On the target, 20 Msps receive through the check tool reached only
  81% with 265 overflows while USB alone carried the full rate: find
  where the CPU goes (the check tool's spectrum calls `cos()` per sample;
  the module converts per sample too). Measure CPU per direction and
  format at 2, 8 and 20 Msps (the check tool reporting process CPU
  time), make the conversions vectorise by hand-written loops the
  compiler can auto-vectorise (no library), and add SoapySDR's direct
  buffer access (`acquireReadBuffer` and friends) for CS8, so a
  native-format reader copies nothing. Acceptance: before and after
  table in `docs/design.md`; direct access tests against the fake; full
  duplex at 20 Msps both ways for ten minutes on the target with no
  overflow, underflow or shortfall, or the limit and its cause recorded.

### 12. Quality gates

- [ ] Clang thread-safety annotations on every guarded member with
  `-Wthread-safety` on, and the lock order (control, then side, then
  time) written beside the locks. A `tsan` preset. clang-tidy (clang-tidy-19
  on the target) with a checked-in configuration, clean. A coverage
  report with the core at or near complete line coverage, gaps listed
  and justified. `tools/ci.sh` gains the tsan run and the coverage
  summary. A full review of the diff since 0.1.0, findings fixed or
  recorded. Acceptance: `tools/ci-pi.sh` passing with all of it, and the
  coverage figure in COMPLETED.md.

### 13. Interoperability

- [ ] Run the pair from the applications people use, on the target
  (installing them there with apt), and record what works in
  `docs/compatibility.md`: `SoapySDRUtil --probe` and `--rate`; the
  SoapySDR Python bindings (installed) with a timed loopback script
  under `tools/`; GNU Radio's Soapy source and sink in one full-duplex
  flowgraph; SoapyRemote serving the pair to another process; one
  receive-only application. Each with the version used, what was
  checked, and any quirk. Acceptance: the matrix filled, every failure
  fixed in the module or explained as the application's.

### 14. Trigger origin on the bench

- [ ] Waits on decision 6: a jumper from the RX board's P28 pin 15
  (trigger out) to the TX board's P28 pin 16 (trigger in), the boards
  grounded together through the clock cable. Built, unit-tested against
  the fake, and on 2026-11-09 the start without a jumper failed on the
  real pair as designed, naming the jumper. Acceptance: with
  `--time-origin trigger`, `hackrfduplex-check --timed-bursts` lands
  every burst at one constant offset from the time asked (the origin is
  0 by construction, so the offset is the boards' pipeline difference).

### 15. Certification and soak

- [ ] `hackrfduplex-check --acceptance` runs the hardware battery and
  prints pass or fail per line against the figures in `docs/design.md`:
  receive at 2, 8 and 20 Msps; a tone in each format; a reference switch
  each way; the origin repeatable; timed bursts at +0; low rates;
  `lo_locked`, `ref_locked`, `time_locked`. It transmits only with
  `--transmit`. Then a 24-hour soak on the target: full duplex at 8 Msps
  with timed bursts throughout, the origin holding (no shortfall) or each
  loss reported and recovered with `calibrate`, memory flat.
  Acceptance: the battery passing on this pair, and the soak's figures
  in COMPLETED.md.

### 16. Documentation and the release

- [ ] `docs/usage.md`, a guide for an application writer: setting up the
  pair (clock cable, USB topology, power), what the time origin is and
  where the calibration burst transmits (the receive frequency, at the
  transmit gain set), the transmit lead to allow, the stream statuses
  and what each means, and troubleshooting (no origin found, reference
  switches, shortfalls, low rates). A man page for `hackrfduplex-check`
  installed by CMake and the package (lintian's one warning on the
  `.deb`). README brought to its final form, the status table all ticks
  or named exceptions. Then, on Tom's word (decision 7): version 1.0.0
  in `CMakeLists.txt` and `debian/changelog`, `tools/ci-pi.sh` passing,
  the `.deb` rebuilt and installed on the target, `develop` merged to
  `main`, an annotated tag `1.0.0` on `main`, and `main`, `develop` and
  the tag pushed to `origin`. Acceptance: a newcomer's path from the
  README to a timed loopback works as written from a clean clone on the
  target; `module_version` reads 1.0.0 in `SoapySDRUtil --probe` on the
  installed package.
