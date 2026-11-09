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
tone at +0.0 Hz in every second and 0 overflows and underflows. The bench
is shielded and Tom has given a standing go-ahead for transmit tests for
this work (standing decisions, below); keep TX gain at 0 and the
amplifier off unless an item says otherwise, and leave `clkout=rx` set at
the end of a run so the boards stay on one reference.

Work the items in order. Items 1 to 3 are small and independent and need
no new hardware knowledge. Items 4 to 7 are the hardware time, one
design, built in four steps; "How the time work fits the code" below
holds the decisions already taken so they are not re-derived. Items 8
and 9 close the project out.

## Where things stand

- 2026-11-09: the 2023 code was rewritten to the 2026 conventions and
  built against SoapySDR 0.9.0, libhackrf 2026.01.3 and Catch2 3.16.1;
  51 test cases pass. Both boards run the 2026.01.3 release firmware.
- Verified on the bench: receive at 2, 8 and 20 Msps without an overflow;
  a tone in every sample format received at exactly its sent offset with
  the boards on one reference; a board whose reference changes at stream
  start is retuned and restarted, both ways on both boards
  (TODO/COMPLETED.md, docs/design.md "Measured").
- Not yet built: anything to do with time on the transmit side, sensors,
  tolerance of unknown settings, rates below 2 Msps, packaging.

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

## How the time work fits the code

Decisions for items 4 to 7, taken 2026-11-09, so the next session builds
rather than designs. Where the code disagrees with a note here, the code
is right and the note is corrected.

- **Positions.** `RxPath::read` returns `RxRead::position`, the sample
  index of the first delivered sample since activation; `RxPath::received()`
  is the board's count so far, dropped included. `TxPath::sent()` counts
  samples handed to the board, zeros included. Add to `TxPath` a
  `queued()` count: samples the writer has committed to the stream
  (published blocks plus the partial one), which is where the next
  written sample will land. All three are on their own board's sample
  clock; with `clkout` set they tick together.
- **The origin.** One signed number in `Pair`, `tx_of_rx_`: the transmit
  position that is on the air at the same instant as receive position 0,
  as `std::optional<std::int64_t>`. Empty means no common origin
  (`hasHardwareTime` false). A transmit time T maps to transmit position
  `rx_position(T) + tx_of_rx_`.
- **Hardware time.** `getHardwareTime()` is `RxPath::received()` plus a
  `time_offset_` set by `setHardwareTime`, converted with
  `SoapySDR::ticksToTimeNs` at the receive rate. Receive timestamps add
  the same offset. `hasHardwareTime("")` is true while `tx_of_rx_` holds a
  value.
- **The burst.** A fixed BPSK sequence of 4096 chips from a 13-bit
  maximal-length shift register, one chip per sample, amplitude 0.7, so
  its autocorrelation peak stands 30 dB over its sidelobes at any rate
  (test this property in isolation). Sent by `Pair` through its own
  `TxPath::write`, noting `queued()` just before as the burst's transmit
  position. Found by a tap on `RxPath`: an optional handler called from
  `on_transfer` with the transfer's position and bytes, installed only
  during calibration, that copies samples into a buffer long enough for
  the burst plus the longest plausible delay (the ring depth plus
  libhackrf's four transfers, in samples). Correlate at native int8
  precision on the host thread after the tap has filled; the peak's
  receive position less the burst's transmit position gives `tx_of_rx_`.
  Accept only a peak at least 20 dB above the median of the correlation;
  otherwise leave `tx_of_rx_` empty and log the level found.
- **Retune for the burst.** If the two directions are tuned apart, set
  the transmitting board to the receive frequency, send the burst, set
  it back. The firmware skips a tune to the frequency it last applied,
  so when the two are already equal, do not retune at all. Sample
  positions run on through both retunes.
- **Where calibration runs.** In `Pair::activate`, after the second of
  the two streams starts and its reference-switch handling is done, on
  the calling thread, before `activate` returns. The application's read
  thread may already be running on the receive stream; the tap does not
  disturb it. A timed `write` arriving while `tx_of_rx_` is empty waits
  on a condition variable up to its timeout, then returns
  `SOAPY_SDR_TIMEOUT`.
- **Losing the origin.** `Board` gains `shortfalls()`, the board's own
  dropped or late sample count (`hackrf_get_m0_state`,
  `hackrf_m0_state::num_shortfalls`, USB API 1.06 and later; the fake
  gets a settable counter). `Pair` reads both boards' counts at the end
  of calibration and again from `readStreamStatus` and from `read` at
  most every 100 ms; a count that moved clears `tx_of_rx_`, logs it,
  and makes the next `readStreamStatus` report `SOAPY_SDR_TIME_ERROR`
  once. Timed writes then return `SOAPY_SDR_TIME_ERROR`. The `calibrate`
  setting (write any value) runs the burst again on demand.
- **Trigger.** `Board` gains `set_hardware_sync(bool)` (hackrf_set_hw_sync_mode).
  With `time_origin=trigger`, `Pair::activate` of the first stream only
  arms and records; both boards start in `activate` of the second: the
  transmitting board first, in sync mode, then the receiving board,
  whose start emits the edge. `tx_of_rx_` is then 0 by construction. The
  transmitting board not streaming within 500 ms after the receiving
  board started means no edge arrived: stop both, throw a BoardError
  naming the P28 pin 15 to pin 16 jumper. Deactivating either stream
  leaves the other running; the origin is lost when either restarts.
- **Timed transmit.** In `TxPath::write`, a target position: if below
  `queued()` the write is late, return a distinct result the device maps
  to `SOAPY_SDR_TIME_ERROR` and `Pair` records for `readStreamStatus`;
  otherwise write zeros from `queued()` up to the target (through the
  normal blocking ring writes, in transfer-sized pieces), then the
  samples. Zeros written this way are not an underflow, and neither is
  the silence after `SOAPY_SDR_END_BURST`, which `TxPath` already
  handles. `SOAPY_SDR_ONE_PACKET` is `END_BURST` on the same call.
- **SoapySDR surface.** `hasHardwareTime`, `getHardwareTime`,
  `setHardwareTime`; `listSensors`, `getSensorInfo`, `readSensor` per
  channel for `lo_locked`; settings `time_origin`, `calibrate`; the
  `time_origin` device argument read in the `SoapyDevice` constructor.
  `readStreamStatus` gains `SOAPY_SDR_TIME_ERROR` beside
  `SOAPY_SDR_UNDERFLOW`, each reported once per event.
- **The check tool.** `--time-origin`, and `--timed-bursts DELAY_US`: a
  mode that reads, stamps a short tone burst at each receive timestamp
  plus the delay, and reports where each burst arrived against where it
  was asked for, with counts of late and time errors. It is the bench
  acceptance test of items 4, 6 and 7.

## Items

### 4. A common time origin

- [ ] The `time_origin` setting and device argument, `burst` by default:
  the burst, the tap, the correlation, the retune around the burst, the
  wait for calibration, shortfall polling, and the `calibrate` setting,
  as "How the time work fits the code" lays out. `trigger` as laid out
  there, built but marked untested until the jumper exists. `none`
  leaves `tx_of_rx_` empty. Acceptance: tests against the fake board for
  the burst's autocorrelation, the correlation finding a burst at a
  known offset in noise, the retune rule, the wait, the shortfall
  invalidation and `calibrate`; on the bench with `burst`, ten starts
  give the same `tx_of_rx_` to within two samples, the figure recorded
  in `docs/design.md`, also with the two directions tuned 45 MHz apart.
  `trigger` is accepted on the bench only when the jumper is fitted;
  until then its acceptance is the unit tests and a note here.

### 5. Hardware time API

- [ ] `hasHardwareTime`, `getHardwareTime`, `setHardwareTime` as laid out
  above; receive timestamps carry the same offset. Acceptance: tests for
  the conversions, the offset and the false case; on the bench the
  hardware time read between two reads lies between their timestamps.

### 6. Timed transmit

- [ ] `writeStream` with `SOAPY_SDR_HAS_TIME` as laid out above: placement
  at the target position with zeros before it, late writes refused with
  `SOAPY_SDR_TIME_ERROR` and reported once through `readStreamStatus`,
  `END_BURST` and `ONE_PACKET`, bursts back to back and with gaps with no
  underflow between them. `hackrfduplex-check --timed-bursts`.
  Acceptance: tests against the fake board for each behaviour; on the
  bench, bursts stamped at each receive timestamp plus a delay of four
  USB transfers arrive at receive position plus `tx_of_rx_` every time
  for a minute, with no late or time-error reports.

### 7. Transmit lead time

- [ ] With `--timed-bursts`, the smallest delay between a receive
  timestamp and a transmit time that gives no late reports over ten
  minutes, at 2, 8, 11.52, 15.36 and 20 Msps, at the default `buffers`
  and at 4. Acceptance: the table in `docs/design.md` "Measured", the
  `buffers` default revisited in its light, and the README's hardware
  time paragraph stating the lead a writer should allow.

### 8. Report the firmware's reference-switch behaviour upstream

- [ ] The firmware switches clock source after tuning and does not
  retune, and caches the applied frequency so a retune to the same value
  is skipped ([docs/design.md](../docs/design.md) "Reference switches").
  Draft an issue for Great Scott Gadgets with the reproduction, for Tom
  to file if he wants. Acceptance: the draft as `TODO/upstream-reference-switch.md`,
  nothing posted.

### 9. Packaging and CI

- [ ] Debian packaging for the module and the tool; a build-and-test
  script that runs the format check, the tests and the sanitizer build.
  Acceptance: a `.deb` that installs on Debian trixie and
  `SoapySDRUtil --find` sees the module; the script passing on the
  workstation.
