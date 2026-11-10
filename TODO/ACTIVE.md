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

Work the items in order. Item 2 finishes measuring the hardware time
(items 1 and 3 to 12 are done), items 13 to 16 take the module to
the 0.2.0 release ("Release 0.2.0" says what that means). Item 17 waits
on hardware Tom does not have yet.

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
  while USB alone carries the full rate. Item 1 then brought the pair up
  there: receive, reference switches each way, the tone and 3658 timed
  bursts at +0, all clean (COMPLETED.md).
- 2026-11-10: items 4 to 7 accepted on the target (COMPLETED.md): a
  board unplugged mid-stream is a stream error within 0.1 s, a killed
  transmitter stops itself within its underrun limit, the probe lists
  everything as designed, and timed receive windows catch timed bursts
  to the sample. 128 test cases.
- 2026-11-10, later: items 9 and 10 accepted on the target, and a start
  that read the previous stream's M0 state fixed (COMPLETED.md). 146
  test cases. The target's USB carries neither direction cleanly at 20
  Msps, with the stock tools as with the module; full duplex is clean
  through 16 Msps, ten minutes of it at 25% of a core (item 11).
- The upstream report on the firmware's reference switch is drafted in
  `TODO/upstream-reference-switch.md`, for Tom to file if he wants.

## The target's power

- The target rebooted three times on 2026-11-09 and 10: once two
  minutes into a 600 s full-duplex run, once with nothing of this work
  running, and once during the CI's sanitizer tests with the boards
  idle. The journal keeps only the current boot, so no cause is
  recorded. Tom, 2026-11-10, asked whether and how hard to use it: "Yes,
  go for it." So long runs are back; the power is still read before and
  after every run, and another reboot stops hardware work until Tom has
  seen it.
- ThreadSanitizer cannot run on the target: on arm64 it needs a 39, 42
  or 48-bit address space and the Pi 5's 16K-page kernel gives 47, with
  GCC 14 and clang 19 alike. `tools/ci.sh` skips the step there, saying
  so; the workstation still runs it.

Nothing blocks the work. Decided 2026-11-09 and recorded under standing
decisions: the target's bench, its power, the transmit underrun limit,
clock and time sources, optional DSP with liquid-dsp, release 0.2.0
first. Still open: the trigger jumper and a `HACKRF1_R9` board, neither
available for now (Tom, 2026-11-09); the release names both paths as
untested against hardware, and item 17 waits for them.

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
- Dependencies: SoapySDR, libhackrf, Catch2; and liquid-dsp, only in a
  build that asks for the optional DSP (below). Nothing else without a
  decision recorded here.
- The transmit underrun limit is on by default at one transfer's worth,
  with a `tx_underrun_limit` setting; the receive overrun limit stays off
  (Tom, 2026-11-09).
- Clock and time sources map onto the existing settings: clock sources
  `internal`, `rx`, `tx` (as `clkout`), time sources `burst`, `trigger`,
  `none` (as `time_origin`); the settings stay as aliases (Tom,
  2026-11-09).
- Optional DSP (Tom, 2026-11-09: "use liquid-dsp. These features MUST be
  optional at compile time"): software DC removal and I/Q balance
  correction on receive, built only with `-DHACKRFDUPLEX_DSP=ON` and
  liquid-dsp found, off at run time until an application asks. Without
  the option the module neither links liquid-dsp nor claims the
  features; the build without it stays the default.
- The next release is 0.2.0 (Tom, 2026-11-09); 1.0.0, with the SoapySDR
  surface and settings held stable, comes after 0.2.0 has been used.
- Transmit tests: Tom's go-ahead for this work on the shielded bench
  ("you may test autonomously until completion"), which the target's
  bench is ("yes, shielded"), both 2026-11-09. Ask before switching the
  amplifier on. The Radioberry on the target may be used for receive
  tests only.
- The target's power supply may sag under two HackRFs and cannot be
  changed (Tom, 2026-11-09: "Watch that power"). Every hardware run
  reads `vcgencmd get_throttled` before and after and records it; a run
  that saw under-voltage is repeated before any shortfall, overflow or
  lost origin in it is put down to the module.
- Commits with no Claude text, and pushes to `origin develop`, at any
  time (Tom, 2026-11-09). One logical change per commit.

## Release 0.2.0

What "elite" means for this module, and the gate for the tag (0.2.0
first, by Tom's choice; 1.0.0 after it has been used):

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
  follow, and tag `0.2.0` on `main`.
- **Named limits.** What could not be tried on hardware (the trigger
  origin, the `HACKRF1_R9` reference-switch path) is named as such in
  the README and design.

## Items

### 2. Transmit lead time

- [ ] With `--timed-bursts`, the smallest delay from a receive timestamp
  to a transmit time that gives no late reports over ten minutes, at 2,
  8, 11.52, 15.36 and 20 Msps, at the default `buffers` and at 4, on the
  target. Method used so far: step the delay
  up in quarter transfers (131072 samples) from 4.5 with 30 s runs, then
  confirm the first clean one for 600 s, stepping on if it fails; a
  script to do it is a candidate for `tools/` or a check-tool mode.
  Figures so far, from the workstation before the boards moved: 2 Msps
  clean at 5.25 transfers (default buffers) and 5.5 (buffers 4, where
  5.25 had 4 late writes in ten minutes); 8 Msps clean at 5.0 for 30 s.
  On the target, 2026-11-10, 30 s runs, three starts each: 5 transfers
  clean and 4.75 late on every write at 2 and 8 Msps, with the default
  ring and with 4 buffers alike (327.7 and 81.9 ms); the ring depth does
  not move it. The ten-minute confirmations are not done. 20 Msps cannot
  be clean on the target (item 11), so its point is 16 Msps instead.
  Every write is late or none is, and the threshold moves by up to a
  transfer between starts, because it depends on how the two boards'
  transfer boundaries fall: so measure several starts per point and
  state the worst. Acceptance: the table in `docs/design.md` "Measured",
  the `buffers` default revisited in its light, and the README's
  hardware time paragraph stating the lead a writer should allow.

### 13. Quality gates

- [ ] Clang thread-safety annotations on every guarded member with
  `-Wthread-safety` on, and the lock order (control, then side, then
  time) written beside the locks. A `tsan` preset. clang-tidy (clang-tidy-19
  on the target) with a checked-in configuration, clean. A coverage
  report with the core at or near complete line coverage, gaps listed
  and justified. `tools/ci.sh` gains the tsan run and the coverage
  summary. A full review of the diff since 0.1.0, findings fixed or
  recorded. Acceptance: `tools/ci-pi.sh` passing with all of it, and the
  coverage figure in COMPLETED.md.
  Done on the workstation, 2026-11-10, waiting on the target's CI: the
  annotations (`mutex.hpp`, Clang's `-Wthread-safety` clean, a Clang
  build in `tools/ci.sh` where GCC is the compiler; the analysis found
  one unlocked read, `bias_tee()`); the tsan preset (runs on the
  workstation only, item notes above); 33 more tests from the coverage
  gaps and the review, 179 in all. Local source coverage (clang): the
  core 99.05% of lines, the SoapySDR translation 92.1%, now counted;
  the gaps left are enum fallbacks that cannot be reached, a ring that
  reports room and then has none, error handlers that ignore a failing
  stop, and refusals in `soapy_device.cpp` still to test. The review
  (one independent reader of all of `module/src` and the check tool;
  there is no `0.1.0` tag, `main` holds the 2023 code, so it was the
  whole module) found seven defects, each now fixed with a test written
  first and seen to fail: a poll from one direction could wake the other
  direction's path after a close had freed it (a crash, reproduced); a
  failed retune on a reference switch left the board streaming with no
  stream; a board error during the burst escaped an activation already
  done and left the origin pending for good; the hardware time while
  receive was stopped counted the last run's samples; empty reads and
  writes spun to their timeout; the check tool could terminate with the
  transmitter running; and an origin outlived a deactivate when
  `time_origin` had been set to none. Recorded, not fixed: if
  `HackrfBoard`'s reopen after a failed start fails, the board is left
  without a libhackrf handle and later calls pass a null one (needs a
  hardware fault to reach). Still to do: clang-tidy on the target, the
  lcov figure of record there, and the remaining refusal tests.

### 14. Interoperability

- [ ] Run the pair from the applications people use, on the target
  (installing them there with apt), and record what works in
  `docs/compatibility.md`: `SoapySDRUtil --probe` and `--rate`; the
  SoapySDR Python bindings (installed) with a timed loopback script
  under `tools/`; GNU Radio's Soapy source and sink in one full-duplex
  flowgraph; SoapyRemote serving the pair to another process; one
  receive-only application. Each with the version used, what was
  checked, and any quirk. Acceptance: the matrix filled, every failure
  fixed in the module or explained as the application's.

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
  or named exceptions, the untested hardware paths named. Then: version
  0.2.0 in `CMakeLists.txt` and `debian/changelog`, `tools/ci-pi.sh`
  passing, the `.deb` rebuilt and installed on the target, `develop`
  merged to `main`, an annotated tag `0.2.0` on `main`, and `main`,
  `develop` and the tag pushed to `origin`. Acceptance: a newcomer's path
  from the README to a timed loopback works as written from a clean
  clone on the target; `module_version` reads 0.2.0 in `SoapySDRUtil
  --probe` on the installed package.

### 17. Trigger origin on the bench

- [ ] Waits on hardware Tom does not have yet (2026-11-09): a jumper from the RX board's P28 pin 15
  (trigger out) to the TX board's P28 pin 16 (trigger in), the boards
  grounded together through the clock cable. Built, unit-tested against
  the fake, and on 2026-11-09 the start without a jumper failed on the
  real pair as designed, naming the jumper. Acceptance: with
  `--time-origin trigger`, `hackrfduplex-check --timed-bursts` lands
  every burst at one constant offset from the time asked (the origin is
  0 by construction, so the offset is the boards' pipeline difference).
