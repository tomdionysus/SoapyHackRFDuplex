# Completed work

Newest first. Each entry is the item as it stood in ACTIVE.md, with its
date and what the acceptance test showed.

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
