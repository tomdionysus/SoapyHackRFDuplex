# Completed work

Newest first. Each entry is the item as it stood in ACTIVE.md, with its
date and what the acceptance test showed.

## 2026-11-12: amd64 packages and the 0.2.1 release

The first clean build, in a `debian:trixie` container for amd64
(`tools/deb-amd64.sh`), stopped at configuration: Debian's
`libhackrf-dev` names `/usr/include/libusb-1.0` in its pkg-config file
without depending on `libusb-1.0-0-dev`, which the Pi and the CI image
already had. `libusb-1.0-0-dev` added to `Build-Depends` and to the
build instructions.

The container's build then failed two time tests, now and then. Both
were the fake air (a transfer every 20 us) outrunning a busy host: a
receive overflow swallowed a timed pulse, and a stall lost the
calibration burst, which was queued only one transfer ahead of the
transmitter. A 200 us stall injected there failed 19 of the 46 time
tests, so the module now queues the burst eight transfers ahead (52 ms at
20 Msps; a new test pins it), the air moves a transfer a millisecond, the
timed-write test resends a pulse lost to a stall or an overflow, and the
wake test no longer assumes its waiter starts within 5 ms. In the amd64
container, 30 runs of the suite at six at once without a failure; one
failure in 10 with six busy loops alongside (an 8 ms stall). On the
pair, 3657 of 3657 timed bursts at +0 samples, power `0x50000`
throughout.

Version 0.2.1; `tools/ci-pi.sh` passing (190 test cases). The
tomcully.com apt repository takes amd64 as well as arm64.

## 2026-11-12: documentation and the 0.2.0 release

Item 16, accepted on the target. `docs/usage.md` and the
`hackrfduplex-check` man page (installed by CMake and the tools
package); the README cut to a front page in corvus-radioberry's form,
its reference material moved to `docs/reference.md`, `building.md`,
`status.md` and a `quickstart.md`, all listed in `docs/index.md`.
Version 0.2.0 in CMake and `debian/changelog`; `tools/ci-pi.sh` passing
(189 test cases, 199 with the receive corrections, clang-tidy clean,
97.1% of lines). The quickstart followed from a fresh anonymous clone on
the target: the packages built with their tests, lintian clean, the
pair found, receive at 99.9% with 0 overflows, the tone at +0.0 Hz,
3658 timed bursts all on time, the man page installed. `develop` merged
to `main` with `--no-ff` (two 2023 commits on `main` changed files the
rewrite removed; the merge keeps the removal, and the tree equals
`develop`'s), annotated tag `0.2.0`, `main`, `develop` and the tag
pushed. Then a fresh clone of the release, built and installed:
`module_version` reads 0.2.0 in `SoapySDRUtil --probe`, timed bursts all
on time. Packages removed afterwards; power `0x50000` throughout.

## 2026-11-12: interoperability

Item 14, accepted on the target: every application in the matrix
(`docs/compatibility.md`) run against the pair, each failure fixed in
the module or put down to the application.

- `SoapySDRUtil --find`, `--probe`, `--rate`: as designed.
- Python bindings, `tools/timed_loopback.py`, 30 bursts at each of 2, 8
  and 16 Msps: every burst found; +0 at 2 and 8 Msps, at 16 Msps 16 at +0
  and 14 one sample early, the script's own finder rounding a delay near
  half a sample the other way from the module's. `Device(dict)` finds no
  device for any driver: the bindings' quirk.
- GNU Radio 3.10.12, the duplex flowgraph: the tone where it was sent at
  2, 8 and 16 Msps, no overflow with default arguments now the receive
  ring holds 1.5 s (it needed `buffers=40` at 16 Msps before).
- SoapyRemote 0.5.2: fixed in the module. Its server reads from
  `setupStream` on and passed every `STREAM_ERROR` the module gave for a
  stream not yet started to its client (30 to 52 a run; `SoapySDRUtil
  --rate` quit at the first); such a read now times out, as other
  drivers' do, and SoapyRemote's own keys pass without warnings. Then
  99.3 to 99.8% received over UDP and TCP with no error, `--rate` each
  way, the duplex flowgraph's tone in place, all with the host's 208 KiB
  socket limit; over UDP a few transmit packets are marked lost, none
  with `remote:prot=tcp`.
- rtl_433 25.02 from Debian, the receive-only application: opens, tunes,
  streams and exits cleanly; it read a 0 Hz filter before the device
  opened at 10 Msps (now 7 MHz); its 0 to 116 dB per-element gain listing
  is its own (it asks for the overall range per element).

## 2026-11-12: certification and soak

Item 15, accepted on the target. The battery, `hackrfduplex-check
--clkout rx --acceptance --transmit`, passed 16 of 16 again with the
receive ring at 1.5 s and no start-up overflow allowed (entry below for
its lines). The soak, `tools/soak.sh`, on the target's unchanged supply:

- 8 Msps, 2026-11-11 21:11 to 23:11 UTC: 439,454 bursts written,
  439,449 found (the last 5 in flight), every one at +0; no late write,
  overflow or underflow; origin held; memory flat at 61.7 MB.
- 16 Msps, first run: clean for 1,020 s, then the TX board stopped
  streaming, a power sample read under-voltage (0x50005), and the host
  reset itself about 50 minutes later. `--recover` could not bring the
  board back, since only deactivating and activating the streams does;
  it now does that.
- 16 Msps, repeat, 2026-11-12 00:10 to 02:10 UTC: 878,906 written,
  878,901 found, every one at +0; 0 overflows, 0 underflows, origin held,
  memory flat at 62.9 MB; 1 late write, in the same two seconds as a
  kernel under-voltage event, from a fresh read. Accepted with the late
  write put down to the supply (Tom, 2026-11-12).

## 2026-11-12: the acceptance battery

The battery half of item 15, accepted on the target:
`hackrfduplex-check --clkout rx --acceptance --transmit` passed 16 of 16
(power 0x50000 before and after; six brief kernel under-voltage events
during it, none touching a line):

- receive at 2, 8 and 16 Msps, 100.0% of nominal, 0 overflows; at 0.2
  Msps 98.0% over 10 s (a transfer is 655 ms there, so one transfer of
  slack is allowed); 20 Msps reported, not judged (99.9%, 95 overflows,
  the target's limit);
- the reference switch to the boards' own clocks and back, ref_locked
  following, 0 overflows, the TX board's switch handled, tx lo_locked;
- a tone at 2 Msps in CS8, CS16, CF32 and CF64, every second judged at
  +0.0 Hz, levels within 0.10 dB (-43.4 to -43.5 dBFS);
- the origin repeatable over 6 findings, spread 0 samples;
- time_locked, ref_locked and lo_locked with both streams running;
- timed bursts, 3662 at 8 Msps and 3662 at 16 Msps, every one at +0;
- transmit and receive at 1.2 Msps, tone offset spread 0.0 Hz.

The soak's figures are under item 15 in ACTIVE.md until it is accepted.

## 2026-11-10: quality gates

Item 13, accepted on the target: `tools/ci-pi.sh` passes with all of it.

- Thread-safety annotations on every lock-guarded member
  (`module/src/mutex.hpp`), `-Wthread-safety` clean, proved by a
  clang++-19 build in `tools/ci.sh`; the lock order beside the locks and
  in `docs/design.md`, Threads. The analysis found `bias_tee()` reading
  unlocked.
- A `tsan` preset; ThreadSanitizer runs on the workstation only, since
  the target's 16K-page kernel gives a 47-bit address space it cannot
  use (GCC 14 and clang 19 alike), and `tools/ci.sh` says so there.
- clang-tidy-19 with a checked-in `.clang-tidy`, clean, run by
  `tools/ci.sh`; it found 45 things, all fixed (optionals read after a
  check it could not see, empty catches now each saying why, widening
  and narrowing, floating loop counters, missing const).
- Coverage on the target (lcov, gcov with atomic counters, which the
  threaded tests need): 96.8% of 2121 lines and 98.4% of functions over
  the unit-tested sources, the SoapySDR translation now included; the
  core 98.8%, the translation 92.7%. Left out: the libhackrf binding and
  the SoapySDR registration, which only hardware and SoapySDR's loader
  run. The core's gaps are enum fallbacks that cannot be reached, a ring
  that reports room and then has none, and error handlers that ignore a
  stop that fails after a failure; the translation's are refusals of
  odd arguments.
- A review of the whole module (no `0.1.0` tag exists; `main` holds the
  2023 code), by an independent reader: seven defects, all fixed with a
  test written first and seen to fail; one recorded and not fixed, a
  failed reopen in `HackrfBoard` leaving no libhackrf handle (needs a
  hardware fault to reach).
- 179 test cases, 189 with the receive corrections, from 146.

## 2026-11-10: transmit lead time

Item 2, accepted on the target. `tools/lead-sweep.sh` at 2, 8, 11.52,
15.36 and 16 Msps (20 Msps cannot be clean on the target, item 11), the
default ring and `buffers=4`, three 30 s starts per quarter-transfer
step from 4.5, the first clean step confirmed for 600 s: 5 transfers at
2 Msps, 4.75 at 8 and 11.52, 4.5 at 16 (the shortest tried), all
confirmed clean for ten minutes; at 15.36 no write late at any step, but
a burst found one sample early about 28% of the time, the loop delay
there falling about half a sample from a whole one. The ring depth does
not move the lead, so the default stays; writers are told to allow 5.5
transfers. Table in `docs/design.md`, "Transmit lead on the target".
Power `0x50000` throughout.

## 2026-11-10: optional receive corrections

Item 12, accepted on the target. With `-DHACKRFDUPLEX_DSP=ON` and
liquid-dsp (1.7.0 on the target, 1.8.2 on the workstation) the receive
channel takes `setDCOffsetMode`, `setDCOffset`, `setIQBalance` and
`setIQBalanceMode` (and their getters); off until set; applied on the
reader's thread in the application's format; the calibration tap and
direct access see raw samples. Without the option none of it is
compiled, liquid-dsp is not linked and the calls refuse as before;
`tools/ci.sh` builds and tests both ways (179 test cases without, 189
with). On the target, full duplex at 8 Msps with a 100 kHz tone: the
centre bin -35.9 dBFS without, -75.1 with DC removal; the tone's image
32.7 dB below it without, 37.2 dB with I/Q balance, 39.2 dB with both, at
the noise floor. The hardware run found the automatic I/Q estimate led
astray by the receiver's DC (b = -0.48 + 0.10j, the image 6 dB down): it
now measures about each read's mean, with a test that failed first. Cost
at 20 Msps receive, CF32: 4% of a core without, 58% with both. Figures
in `docs/design.md`, "Receive corrections". Power `0x50000` throughout.

## 2026-11-10: performance

Item 11, accepted on the target with 20 Msps recorded as the host's
limit. The check tool reports process CPU and receives in any format or
through direct buffer access (`--rx-format`, `--direct`,
`--no-analysis`); a CS8 receive stream offers SoapySDR direct buffer
access (`[direct]` tests); `convert.cpp` is built at -O3, where GCC
vectorises its loops (float to native 5.1 to 1.4 ns a sample, CS16 2.0
to 0.5), and a test pins the float rounding over every tie. The table
and the conversions' timings are in `docs/design.md`, "Rates and CPU on
the target". Receiving 20 Msps costs the module 2 to 5% of a core;
full duplex at 16 Msps 25 to 30% for the whole process. The 20 Msps
limit is the target's USB: `hackrf_transfer` alone with stock libhackrf
moves only 36.7 to 37.0 MB/s of 40 transmitting from either board, with
a board shortfall per 16 KiB, and receives with 977 to 5633 shortfalls
in 4 s; it transmits cleanly at 16 Msps and below. So the ten minutes
were run at 16 Msps instead: full duplex in CF32, 9.60 G samples
received at 100.0% of nominal, one overflow (the activation backlog),
no shortfall, 0 underflows, the origin held, 25% of one core. At 18
Msps the transmitter falls short. The first 81% at 20 Msps was the check
tool's analysis (95% of a core alone; the module 5%). Power `0x50000`
throughout.

## 2026-11-10: a start no longer reads the last stream's M0 state

Found on the target while measuring item 10, fixed in `95872c8`. The
firmware acknowledges a start from its USB interrupt and its M0 zeroes
the shortfall count and error only when it takes the start up, a few
milliseconds later; moving to idle never zeroes them
(`sgpio_m0.s`, "Don't reset counts on a transition to IDLE"). A poll in
between read the previous stream's count, even another process's, then
the zero, and the pair failed the board as restarted under the stream.
After a 20 Msps run that left 1477, 1430 and 1423 shortfalls on the RX
board, three 2 Msps starts in three failed naming exactly those counts;
after a clean 8 Msps run, the same start was clean. `HackrfBoard` now
returns from a start once the M0 has left idle or its counts have moved
(`m0.hpp`, tested), and from a stop once the M0 is idle, each within
500 ms. With the wait, after stale counts of 1507, 1463 and 1434, three
starts in three found their origin. The same wait covers the error a
self-stop leaves (item 6's kill test leaves 2). The origin at start moved
by about 2 ms of samples (+4765 to +8765 at 2 Msps), the starts' new
spacing; timed bursts still land at +0 (1217 at 8 Msps, 302 at 2 Msps).
Before item 4 made a count that goes back fatal, the stale count read as
a shortfall and lost the origin at start instead. Power `0x50000`
throughout.

## 2026-11-10: bounded activation, calibration ends early

Item 10, accepted on the target. Calibration correlates the capture as
it fills (`burst::Finder`) and stops once a peak clears the threshold
with a burst's length of correlation past it. Tests: `[burst]` Finder
cases, and "calibration stops listening once the burst is certain"
against the fake. On the target, the second activation (full duplex, a
100 kHz tone at TX gain 0, `--no-analysis`), three starts each, the
module at `ace0dd8` against the module after `111c15e`, both through
today's check tool:

| Rate | Before (s) | After (s) |
|---|---|---|
| 2 Msps | 2.46, 2.46, 2.46 | 0.58, 0.65, 0.59 |
| 8 Msps | 1.17, 1.25, 1.16 | 0.24, 0.29, 0.38 |
| 20 Msps | 1.00, 1.02, 1.00 | 0.65, 0.76, 0.51 |

At 20 Msps neither finds an origin: both boards fall short of the host
at that rate (item 11), so the burst is never trusted. Ten calibrations
in one run at 8 Msps: +18167 every time, spread 0 samples over 11
findings; three more runs of three each at 2 and 8 Msps, spread 0. The
remaining activation time is the transmit ring draining ahead of the
burst, which is placed behind everything already queued. Power
`0x50000` throughout.

## 2026-11-10: device sensors ref_locked and time_locked

Item 9, accepted on the target. `listSensors()` gives `ref_locked` (the
boards share one reference, from the CLKIN and CLKOUT state each board
had when last idle) and `time_locked` (an origin holds), booleans read
without touching the boards. Tests: `[sensors]`, four cases. On the
target, receive only: with `--clkout off`, `ref_locked` false before,
during and after (TX on its own crystal); with `--clkout rx`, true
throughout. Full duplex with `--clkout rx`: `time_locked` false before
the start, true while both streams run, false once they stop. Power
`0x50000`.

## 2026-11-10: timed receive windows

Item 7, accepted on the target. `activateStream` on the receive stream
takes `SOAPY_SDR_HAS_TIME` (deliver from that time, the first read
stamped exactly at it) and `SOAPY_SDR_END_BURST` with `numElems` (that
many samples, the last read flagged), on a stopped or a running stream;
a time already passed is `SOAPY_SDR_TIME_ERROR`. Tests: six cases,
`[timed-rx]`. On the target, `hackrfduplex-check --clkout rx --rate 2e6
--timed-window 500000 --seconds 20`: 39 rounds, 0 refused, 0 short, 0
without END_BURST, 0 stamped other than asked, every burst found at +0
samples, at the start of its window to the sample. Power `0x50000`
before and after.

## 2026-11-10: the transmitter cannot outlive its host

Item 6, accepted on the target. The underrun limit is set before every
transmit start (one transfer's worth unless `tx_underrun_limit` says
otherwise, also a device argument) and set back to 0 after a clean stop;
a board that stopped itself is a stream error; old firmware is named in
the log; the transmit audit finds the fake transmitter handed exactly
the written samples, the calibration burst and zeros. Tests:
`[safety]`. On the target, `tools/tx-kill-test.sh` (new): the check
tool transmitting a 200 kHz tone at 2 Msps, TX gain 0, killed with
SIGKILL after 5 s. The TX board's M0 then read: active mode IDLE, error
2 (TX_TIMEOUT), one shortfall of 262144 bytes, which is the limit (one
transfer, 65 ms at 2 Msps); listening on the RX board alone 1.6 s after
the kill, the tone's bin was 1.2 dB over the median, so nothing. A
receive-only run straight after reopened the pair cleanly (99.7%, 0
overflows). The margin: at 20 Msps full duplex the check tool starves
the transmitter (19.46 Msps sent, 206 underflows, about 230 board
shortfalls a second for 20 s) and the board never stopped itself, so
no single stretch reached one transfer (6.5 ms); the default stands.
Power `0x50000` throughout.

## 2026-11-10: refuse what is not supported

Item 5, accepted on the target. Every SoapySDR entry point audited: a
stream flag the device cannot honour returns `SOAPY_SDR_NOT_SUPPORTED`,
any other call it cannot honour throws naming why, neutral values of
the corrections are accepted, unknown stream arguments and settings
are logged. The table is in docs/design.md, "The SoapySDR API against
the module". Tests: `[refuse]`. The probe on the target showed one
more: `clkout=rx` given as a device argument was dropped silently, the
reference left off. Now every setting but `calibrate` is also a device
argument, and an unknown device argument is logged (two new cases). The
probe then: `clock out: rx` logged, `rx_clkout=on`, both boards `clock:
external`, clock sources `internal, rx, tx`, time sources `burst,
trigger, none`, the `tx_underrun_limit` setting listed, `lo_locked`
true on both channels; otherwise as before. This also accepts item 8's
probe of the sources.

## 2026-11-10: lost samples and lost boards are reported

Item 4, accepted on the target. Shortfalls are polled whenever a stream
runs, at most every 100 ms: a receive shortfall is one
`SOAPY_SDR_OVERFLOW` with `SOAPY_SDR_END_ABRUPT`, a transmit shortfall
one `SOAPY_SDR_UNDERFLOW`, either loses the origin; a board that has
stopped streaming, or fails a libhackrf call, is a stream error. Tests:
`[fault]`. On the target, `tools/unplug-test.sh` (new) deauthorises a
board's USB device mid-run and puts it back: the RX board receiving at
2 Msps gave `SOAPY_SDR_STREAM_ERROR` 0.10 s after the unplug, the TX
board transmitting a tone 0.04 s after; both boards back on the bus
afterwards. Two fixes came out of the runs. At 20 Msps full duplex the
shortfall warning came at every poll, about 20 lines a second; the log
now names the first at once and the rest at most once a second with
their count, and any left when the stream stops. The first unplug then
logged "4294960398 more shortfalls": the count read 6898 as the board
left and then 0. A count that goes back means the board restarted
under the stream, and is now a failed board. 128 test cases. The check
tool now ends a run on a stream error from either side and prints when
it came. Power `0x50000` throughout.

## 2026-11-09: clock and time sources through the SoapySDR API

Item 8. `listClockSources`, `setClockSource`, `getClockSource` map onto
`clkout` (`internal` for off, `rx`, `tx`) and `listTimeSources`,
`setTimeSource`, `getTimeSource` onto `time_origin` (`burst`, `trigger`,
`none`); both settings stay. Calibration is refused, with a log line
naming `clkout`, unless the boards share a reference: each board's CLKIN
and CLKOUT are read while it is idle at its activation (the clock chip,
and on the older boards the CLKIN status with it, reads as zero while a
board streams), and CLKOUT changes through the pair are followed.
Tests: the two mappings, the refusal with each board on its crystal and
with a fed board whose source is not driven, each board driving the
other, and CLKOUT switched off under running streams; the time-test
rigs now put both boards on CLKIN, as the bench is. 124 test cases. The
probe listing the sources waits for the boards, with item 5's.

## 2026-11-09: calibration restores the transmit frequency the pair holds

Item 3. When the transmitter was retuned to the receive frequency for
the calibration burst, the frequency put back afterwards was the one
noted before the burst, so a transmit tune made meanwhile was lost on
the board while `getFrequency` reported it. Now the restore applies the
frequency the pair holds at that moment, under the transmit side's lock.
Test: "a transmit tune during the burst is what the board keeps" tunes
the transmitter from a second thread while the burst is out (the
loopback paused until the tune lands) and finds board and pair both on
the new frequency; it failed before the fix with the board on the old
one. 97 test cases, the time tests 20 times over without a failure.

## 2026-11-09: the pair brought up on the target

Item 1, accepted on the target (`corvus-macnessa-sdr`, a Raspberry Pi 5,
Debian 13.7 trixie, each board on its own root port). `vcgencmd
get_throttled` read `0x50000` before and after every run: under-voltage
had happened since boot, none during these runs.

- Receive only, `--clkout rx --seconds 10`: 2 Msps 99.9% of nominal and
  8 Msps 99.9%, 0 overflows, 0 timeouts, both boards `clock: external`.
- Reference switches, with the 100 kHz tone at TX gain 0: CLKOUT off and
  on alternately, four starts, each logging "TX board changed reference
  clock; retuned and restarted" with `lo_locked` true on both channels
  after it. On the crystal the tone arrived at +87.9 kHz (the TX
  crystal's 15 ppm) and the burst was not heard (8.2 to 9.1 dB), as
  documented for free-running boards; on CLKIN it arrived at +0.0 Hz
  and the origin was found (36.8 and 37.2 dB). The check tool now
  reports `lo_locked` per channel after activation.
- Smoke test, `--clkout rx --tx-tone 100e3 --tx-gain 0 --seconds 10`:
  origin found at 36.8 dB, the tone at -44 dBFS and +0.0 Hz in every one
  of 10 seconds, image 31 dB down, 0 underflows, 1 overflow (the
  calibration backlog).
- `--timed-bursts 81920 --seconds 60`: 3663 written, 3658 of 3658 found
  at +0 samples, 0 late writes, 0 time errors, 0 underflows, the hardware
  time inside its reads 3662 of 3662; origin at 44.1 dB.
- Not done: the optional check through the Radioberry.
- `docs/design.md` "Measured" now names the host class of every table
  and opens with the target's figures.

## 2026-11-09: the build moves to the target; packaging and CI

The build of record moved to a Raspberry Pi 5 on Debian 13.7 trixie
(arm64) with both boards attached, each on its own USB root port.
`tools/ci-pi.sh` (new) copies the working tree there over ssh and runs
`tools/ci.sh` or any command in the copy; the target's address lives in
`CLAUDE.local.md`. Installed there: hackrf and libhackrf-dev 2024.02.1,
libsoapysdr-dev, soapysdr-tools and python3-soapysdr 0.8.1, debhelper,
lintian, clang-format 19.

Item 9, packaging and CI, accepted:

- `tools/ci.sh` (format check, licence lines, release and sanitizer
  builds and tests) passes on the workstation and on the target: 43
  files formatted, 51 licence lines, 96 test cases in each build, with
  LeakSanitizer on Linux. It now fails rather than passing vacuously
  when git lists no files (it had, on a tree git refused for its owner).
- Every source compiles under GCC 15 (workstation) and GCC 14 (target)
  with `-Werror`; one shadowed name fixed.
- `dpkg-buildpackage -us -uc -b` on the target builds
  `soapysdr0.8-module-hackrfduplex` and `hackrfduplex-tools` (arm64),
  running all 96 tests during the build; both install, and
  `SoapySDRUtil --find` lists the pair in both orders. lintian's one
  warning is the missing man page (ACTIVE item 16). The packages were
  removed again so they do not shadow development builds.

On the target, receive only (transmit there waits on Tom): 8 Msps,
99.9% of nominal, 0 overflows, both boards `clock: external` with
`clkout=rx`, a reference switch handled at the first start. 20 Msps
through the check tool: 81%, 265 overflows; `hackrf_transfer` alone
carried 40 MiB/s, so the limit is CPU, not USB (ACTIVE item 11). Without
a trigger jumper, `--time-origin trigger` failed on the real pair as
designed, naming the jumper.

## 2026-11-09: the firmware's reference-switch behaviour, drafted for upstream

`TODO/upstream-reference-switch.md`: the issue as it would go to Great
Scott Gadgets, nothing posted. It traces the fault in the 2026.01.3
source (`transceiver_startup` tunes through `radio_switch_mode` before
`activate_best_clock_source` changes the reference, and
`radio_set_frequency` skips a tune to `applied_freq`), gives a
reproduction with stock `hackrf_transfer` and `hackrf_clock`, suggests
choosing the clock source before tuning, and describes the module's
workaround.

## 2026-11-09: a common time origin, the hardware time API, timed transmit

Items 4, 5 and 6, built together since each needed the others' parts:
the calibration burst and its correlator (`burst.*`, `fft.*`), a tap on
the receive path, timed blocks placed by the transmit path's board
thread, the origin in `Pair` with the trigger alternative, shortfall
polling, and the SoapySDR surface (`hasHardwareTime`,
`getHardwareTime`, `setHardwareTime`, timestamps with the offset,
`SOAPY_SDR_HAS_TIME` writes, `ONE_PACKET`, `SOAPY_SDR_TIME_ERROR` in the
stream status, the `time_origin` argument and setting, the `calibrate`
setting). The check tool gained `--time-origin`, `--buffers`,
`--calibrations` and `--timed-bursts`. 37 new test cases, 96 in all,
the time tests run 20 times over without a failure; the trigger origin
is tested against the fake board only.

On the bench, 8 Msps, TX gain 0, amplifier off, boards on one
reference:

- Origin: found at 20 starts of 20, ten tuned alike and ten with the
  transmitter 45 MHz below the receiver (retuned for the burst and
  back), 27.5 to 30.2 dB over the median. Repeat calibrations within a
  start agreed to 0 samples every time (three findings in each of the
  20 starts, and ten in a row once each way). The origin itself differs
  per start, 0.1 to 1.6 ms, as the host's start timing does.
- The first burst as planned (0.7 of full scale on I) was not found:
  12.4 dB over the median, buried by the receiver's DC offset. Removing
  the capture's mean and sending the chips at full scale on I and Q
  gave the figures above.
- Timed transmit, `--timed-bursts 81920 --seconds 60` (five transfers):
  3658 of 3658 bursts at +0 samples from the time asked, 0 late writes,
  0 time errors, 0 underflows. At 4.5 transfers every write was late,
  at 5, 6 and 8 transfers none were.
- Hardware time: read between two live reads, it lay between their
  timestamps 3662 times of 3662.

Where the code differed from the plan written before it, the code was
right (docs/design.md, Time, holds the design as built):

- The burst goes out at full scale on I and Q, not 0.7 on I, and the
  capture's mean is removed before correlating (12.4 dB as planned, 27
  to 30 dB as built).
- Timed blocks are placed by the board's transfer thread, not by the
  writer writing zeros up to the target: silence the board inserts into
  an empty ring moves later samples, which only that thread knows.
- The origin is transmit position less receive position, so a transmit
  time T goes out at `rx_position(T) + origin`.
- The origin differs from start to start (it holds how far apart the
  host started the streams), so "ten starts give the same origin" became:
  an origin found at every start, and repeat calibrations within a start
  agreeing.
- The lead a timed write needs is about five USB transfers, not four:
  libhackrf keeps four transmit transfers ahead of the air, and a
  receive timestamp is up to one transfer old when read.

## 2026-11-09: sample rates below 2 Msps

At 1.92 Msps both ways, 10 s: 1.918 Msps received (99.9%), 0 overflows,
0 underflows, the tone at +0.0 Hz every second, -59.5 dBFS. The same at
1.6 and 1.2 Msps.

Stepping down, with `hackrfduplex-check --tx-rate` (new) holding the
other direction at a rate it handles and each board tried in each role:

- Receive is clean on both boards to 200 ksps, the tone at its offset
  every second and no overflow; image rejection drops 2 to 6 dB from
  its 8 Msps figure. The clock chip's divider ends near 195 ksps.
- Transmit is clean at 1.1 and 1.08 Msps. At 1.05 Msps and below the
  stream runs without an underflow but the older board puts out
  nothing and the r10 only one of I and Q (the tone and its image at
  equal level, found with the image report the check tool now prints).

The two directions are advertised apart, which SoapySDR allows:
0.2 to 20 Msps receive, 1.2 to 20 Msps transmit (a margin above the
transmit failure), with 1.92 Msps listed for both. One new test case,
59 in all. Figures in `docs/design.md`, Low sample rates.

## 2026-11-09: LO lock sensor

An `lo_locked` sensor on each channel, a boolean, from `Pair::lo_locked`:
false from the moment `activate` detects a reference switch until the
retune and restart are done (and left false if that fails), true
otherwise. Four new test cases, 58 in all: the two states through the
pair, watched from inside `activate` through a new `FakeBoard::on_call`
hook, and the sensor's listing, info and readings through the SoapySDR
device. On the pair, `SoapySDRUtil --probe` lists `lo_locked` on both
channels, and after tuning both directions to 868.3 MHz both read
`true`.

## 2026-11-09: settings applications write blindly

`writeSetting` with an unknown key logs a warning and returns;
`readSetting` with one returns an empty string. The README says so
under the settings table. To test it, `SoapyDevice` moved into a static
library of its own with a constructor over a ready-made pair, so the
SoapySDR surface is now tested over fake boards (three new test cases,
54 in all). `SoapySDRUtil --probe` on the pair is unchanged apart from
the version string.

## 2026-11-09: reference switches at stream start

The free-running tone that went missing turned out to be a firmware
behaviour, found in the 2026.01.3 source and proved on the bench:

- The firmware picks CLKIN or its crystal at each stream start, after
  tuning, and does not retune; a board whose reference changes goes deaf
  or mute while its sample stream looks healthy. A reset, which boots
  and tunes on the crystal from the start, cured it, which is how the
  cause was separated from a bad crystal.
- It skips a tune to the frequency it last applied, so a plain retune
  did nothing.
- A retune when nothing switched left the clone board's receiver 1.7 kHz
  off, held until that board was reset; that was the 1 Hz-step version.
- Clock chip registers read as zero while streaming, and the clone r10
  board is clocked by its id (`HACKRF1_OG`), not its revision.

The module now asks each board before a start whether the start will
switch its reference, and only then retunes 1 MHz away and back and
restarts the stream. Four new test cases; 51 in all pass. On the bench,
with clean boards, every locked run read 0.0 Hz and every free-running
run showed the crystal's steady 15 ppm, transmitting and receiving, and
the restart fired exactly on the starts that switched
(`docs/design.md`, Reference switches).

## 2026-11-09: transmit in every format

With Tom's go-ahead, `hackrfduplex-check --tx-format` (new) sent the same
tone, TX gain 0, amplifier off, 10 s each, after the clock cable was
shortened:

| Format | Tone at the receiver | Offset | Underflows |
|---|---|---|---|
| CS8 | -59.3 dBFS | 0.0 Hz every second | 0 |
| CS16 | -59.5 dBFS | 0.0 Hz every second | 0 |
| CF32 | -59.5 dBFS | 0.0 Hz every second | 0 |
| CF64 | -59.6 dBFS | 0.0 Hz every second | 0 |

The four agree within 0.3 dB. The tone is 18 dB stronger than in the
first CF32 run because the bench changed between them (the cable swap),
which the CF32 repeat under the same conditions confirms. The image at
-100 kHz sits 20 dB below the tone, the transmitter's I/Q imbalance;
the products at +-200 kHz are 22 dB below.

## 2026-11-09: verify transmit and the shared reference on the air

With Tom's go-ahead: `hackrfduplex-check --clkout rx --tx-tone 100e3
--tx-gain 0 --seconds 10`, CF32, 8 Msps, 868.3 MHz, amplifier off, stub
antennas 20 cm apart. Receive 99.9% of nominal with 0 overflows, transmit
0 underflows, the tone at -77 dBFS and at 0.0 Hz from where it was sent
every second for 9 s, both boards idle afterwards. `hackrfduplex-check`
gained a whole-band spectrum and a per-second tone search to show it.

The acceptance test as written asked for the tone 30 dB above the mean
level; the mean is the whole band's power and the wrong yardstick for one
tone. The tone was 9 dB above the median 977 Hz bin and about 26 dB above
the floor of the 1 Hz search, at the lowest transmit gain.

Found on the way, recorded in `docs/design.md`: the strongest signal in
the band is the 87th harmonic of the 10 MHz reference at 870.000 MHz,
radiated by the clock cable; and turning `clkout` off with the fed board
powered leaves it without a clock (the tone vanished), which became
ACTIVE.md item 2. The shared reference was restored and checked with
`hackrf_clock` afterwards.

## 2026-11-09: shared reference clock

One TCXO module came off the transmitting board and an SMA cable runs
from the receiving board's CLKOUT to its CLKIN. Checked in order with
`hackrf_clock`: the fed board read no clock with CLKOUT off, read a clock
once CLKOUT was enabled (Si5351 CLK3 up at 10 MHz), and the pair received
at 8 Msps with 0 overflows on the shared reference. The module gained the
`clkout` setting (`off`, `rx`, `tx`), read back from each board's clock
chip rather than remembered, with the old source switched off before the
new one comes on; `hackrfduplex-check --clkout` drives it. On hardware,
`--clkout off` left the fed board on its own crystal (`tx_clock:
internal`) and `--clkout rx` put it back on the reference (`tx_clock:
external`, `rx_clkout: on`), with the stream clean both ways. 48 test
cases pass. The on-air confirmation of the lock waits on the transmit
test (ACTIVE.md item 2).

## 2026-11-09: firmware update

At Tom's request both boards were flashed to the 2026.01.3 release
firmware from the Homebrew hackrf package with `hackrf_spiflash -w ... -R`
(compatibility check passed on each). Both re-enumerated on 2026.01.3,
USB API 1.10; the older board, previously on a custom build with API
1.03, now reports its revision (older than r6) and its TCXO as an external
clock, as the r10 already did. `SoapySDRUtil --probe` opens the pair and
`hackrfduplex-check` receives as before.

## 2026-11-09: remove the 2023 files

Tom removed the 2023 sources, packaging, CI file and stale build output
by hand. `cmake --preset default` configured into a clean `build/`, the
build was warning-free and `ctest --preset default` passed 47 of 47.

## 2026-11-09: verify receive on hardware

`hackrfduplex-check --seconds 10` on the attached pair (RX the r10 board,
firmware 2024.02.1), gain 32 dB, 868.3 MHz, stub antenna:

| Rate | Samples in 10 s | Achieved | Overflows | Stream time span | Mean level |
|---|---|---|---|---|---|
| 2 Msps | 20 054 016 | 2.000 Msps (100.0%) | 0 | 9.96 s | -35.9 dBFS |
| 8 Msps | 80 084 992 | 7.999 Msps (100.0%) | 0 | 9.99 s | -35.7 dBFS |
| 20 Msps | 199 884 800 | 19.983 Msps (99.9%) | 0 | 9.99 s | -35.8 dBFS |

No clipping (peak 0.03 of full scale), no timeouts, both boards idle
afterwards. The acceptance test asked for the rate within 1% and 0
overflows; both held even at 20 Msps on a shared USB bus.

## 2026-11-09: build and pass the tests

With SoapySDR 0.9.0, libhackrf 2026.01.3 and Catch2 3.16.1 from
Homebrew: `cmake --build` of the core, the libhackrf binding, the module,
the tests and `hackrfduplex-check` with `-Werror` clean; 47 test cases,
2162 assertions, all passed; `SoapySDRUtil --find="driver=hackrfduplex"`
lists both orderings of the attached pair and `--probe` opens one and
reports both boards. One fix on the way: a missing include in the SoapySDR
translation.

## 2026-11-09: rewrite to the 2026 conventions

The 2023 code (one device class with every member duplicated for RX and
TX, no tests, warnings silenced) was replaced by:

- a core with no hardware or SoapySDR dependency: `format`, `convert`,
  `block_ring`, `gain`, `bandwidth`, `rx_path`, `tx_path`, `pair`,
  `selection`;
- `hackrf_board`, the `Board` interface over libhackrf with a
  reference-counted session and a board list that opens nothing;
- `soapy_device` and `registration`, the SoapySDR translation;
- `hackrfduplex-check`, a SoapySDR client that receives, optionally
  transmits a tone, and reports;
- Catch2 tests for every core component against `fake_board`;
- CMake with presets, sanitizer and coverage builds, a version header
  from `git describe`, and a libhackrf lookup that works beside a
  MacPorts pkg-config;
- GPL-3.0-or-later, README, CONTRIBUTING, design and licensing documents,
  and this plan.

Design decisions taken on the way: discovery never opens a board; the
receive stream carries a sample-count time and reports a gap once as an
overflow before the data after it; the transmit path treats silence
before the first write and after a burst as intended; an overall gain
spends the amplifier last.
