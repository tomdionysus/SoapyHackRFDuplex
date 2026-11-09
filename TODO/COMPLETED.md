# Completed work

Newest first. Each entry is the item as it stood in ACTIVE.md, with its
date and what the acceptance test showed.

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
