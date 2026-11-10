# Design

hackrfduplex presents two HackRF One boards to SoapySDR as one
full-duplex device. This is how it is put together and why.

## Components

```
SoapySDR client
      |
soapy_device.cpp, registration.cpp     SoapySDR::Device and the registry entry:
      |                                translation only
Pair (pair.hpp)                        two Boards, the settings of each direction,
      |            \                   one stream per direction
RxPath             TxPath              the stream paths: transfer-sized blocks in a
      |            |                   ring, conversion to and from the host format
Board (board.hpp)  Board               the interface one HackRF offers
HackrfBoard        HackrfBoard         the interface over libhackrf
```

Everything above `HackrfBoard` and below `soapy_device.cpp` is the core,
`hackrfduplex_core`, and includes neither libhackrf nor SoapySDR; beside
the pair and the paths it holds `burst.*` and `fft.*`, the calibration
burst and its correlator (Time, below). The tests drive it through
`FakeBoard`, which records settings and lets the test deliver and pull
transfers by hand, and drive `SoapyDevice` the same way over a pair of
fakes; `tests/loopback.hpp` plays the air between two fakes for the
time tests.

### Board

`Board` is what the pair needs of one HackRF: tuning, sample rate, filter,
the gain stages, the bias tee, and streaming in one direction at a time
with a handler called on the board's transfer thread. Two facts of the
firmware are part of the interface because the pair depends on them:
setting a sample rate resets the baseband filter to the firmware's
default, and going idle drops the bias tee. `FakeBoard` copies both.

`HackrfBoard` serialises control calls on a mutex and holds one
`HackrfSession`, which reference-counts `hackrf_init` and `hackrf_exit`
across the process. If a start command succeeds but the board does not
report streaming, it closes and reopens the handle and starts once more,
which is the cure the single-board module found for a libhackrf restart
refusal. `stop()` calls both `hackrf_stop_rx` and `hackrf_stop_tx`;
either returns after libhackrf has joined its transfer thread, so no
handler runs after it.

### Pair

`Pair` owns the two boards and, per direction, the tuning in effect and
one optional stream. Settings go to the board at once and are remembered.
A sample rate change re-applies an explicit bandwidth; a bandwidth of 0
hands the filter back to the firmware and the reported bandwidth becomes
the firmware's three-quarters rule. Gains are quantised to the stages'
steps, and one overall gain is split by `gain::split_rx` and
`gain::split_tx`, which spend the two variable stages in proportion to
their ranges and switch the RF amplifier on only when they cannot reach
the request. The bias tee is remembered and re-applied whenever
transmission starts.

The reference clock out (`clkout`) is the one setting not remembered:
it is read back from each board's clock chip, because CLKOUT outlives
the device object and dies with the board's power, and a state kept
here would lie after either. Switching the source turns the old
board's output off before the new one's comes on, so two outputs never
drive one cable.

A stream is opened in a host format with a ring depth, activated to start
the board, and deactivated to stop it. Activation resets the path so a
stream always begins at position 0. The pair's destructor stops anything
running.

### The stream paths

Both paths are built on `BlockRing`, a single-producer single-consumer
ring of preallocated blocks of one transfer each, with timed waits on
either side and a `wake()` that releases them when a stream stops. The
ring takes a mutex per call; at the transfer rates involved (tens to a
few hundred per second per board) that costs nothing measurable, and it
keeps the waits simple.

`RxPath`: the board's thread copies each transfer into a free block
stamped with its stream position in samples. A transfer that finds the
ring full is dropped whole, counted, and still advances the position, so
positions stay true to the board's sample clock. The reader takes up to a
transfer's worth of samples per call, converting into its format. When
the reader reaches a block whose position is not what it expected, the
gap is reported once as an overflow, before the data that follows it.

`TxPath`: the writer fills a block in the board's format, publishing it
when full or when the caller marks the end of a burst, or starts a new
one at a stream position for a timed write (Time, below). The board's thread
concatenates blocks into each transfer. A transfer that runs dry is
padded with zeros and counted as an underflow, unless nothing has been
written yet or the last block ended a burst, in which case silence is
what was asked for. libhackrf asks for the first transfers the moment
transmission starts, which is why silence before the first write is not
an underflow.

### SoapySDR translation

`SoapyDevice` maps the SoapySDR API onto the pair: direction and channel
checks, format names, the gain element names an application such as
gr-osmosdr expects (`LNA`, `AMP`, `VGA` for RX; `VGA`, `AMP` for TX), the
`buffers` stream argument, the `bias_tx` setting, and the error codes.
Stream handles are the addresses of two integers in the device. Discovery
(`registration.cpp`) lists boards without opening them, so a pair held
open in the same process still appears, and offers every ordered pair of
distinct boards unless the arguments name one.

### The SoapySDR API against the module

What the device cannot do it refuses: `SOAPY_SDR_NOT_SUPPORTED` from a
stream call, or `std::invalid_argument` naming why from any other. The
one exception is a setting or stream argument it does not know, which
applications pass around blindly: that is logged as a warning and
ignored. Every per-channel call checks its direction and channel (one
channel, 0, each way).

| Call | Module |
|---|---|
| `getHardwareKey`, `getHardwareInfo` | "HackRF Duplex"; both boards' serial, board, revision, firmware, USB API, clock and CLKOUT, the module and libhackrf versions |
| `getNumChannels`, `getFullDuplex` | 1 each way; full duplex |
| `getStreamFormats`, `getNativeStreamFormat` | CS8 (native, full scale 128), CS16, CF32, CF64 |
| `getStreamArgsInfo`, `setupStream` | `buffers`, the ring depth in transfers; any other argument logged and ignored |
| `activateStream` | starts the board; on receive, `SOAPY_SDR_HAS_TIME` starts delivery at that time and `SOAPY_SDR_END_BURST` with a count ends it after that many samples, also on a stream already running; a time passed `SOAPY_SDR_TIME_ERROR`; any other flag, and any on transmit, `SOAPY_SDR_NOT_SUPPORTED` |
| `deactivateStream` | stops the board at once; flags: `SOAPY_SDR_NOT_SUPPORTED` |
| `readStream` | samples stamped with `SOAPY_SDR_HAS_TIME`; `SOAPY_SDR_END_BURST` on the last read of a receive burst; overflow with `SOAPY_SDR_END_ABRUPT`; a failed board `SOAPY_SDR_STREAM_ERROR` |
| `writeStream` | `SOAPY_SDR_HAS_TIME`, `SOAPY_SDR_END_BURST`, `SOAPY_SDR_ONE_PACKET`; any other flag `SOAPY_SDR_NOT_SUPPORTED` |
| `readStreamStatus` | transmit: underflow, time error, stream error; receive: `SOAPY_SDR_NOT_SUPPORTED` |
| `getStreamMTU` | one transfer of samples |
| `acquireReadBuffer` and the rest of direct access | SoapySDR's: not supported (item 11 adds CS8) |
| antennas | `TX/RX`, the one port |
| gains | overall, and `LNA`, `VGA`, `AMP` (receive), `VGA`, `AMP` (transmit); automatic gain refused |
| `setFrequency`, `getFrequency` | the `RF` component only; tuning arguments refused |
| `setSampleRate`, ranges, lists | passed through; ranges as measured (Low sample rates) |
| `setBandwidth`, ranges, lists | the filter widths, rounding down; 0 or less for the automatic filter |
| DC offset, I/Q balance, frequency correction | none: the neutral value (off, zero) accepted, any other refused (item 12 adds DC and I/Q on receive as a build option) |
| hardware time | the receive count plus an offset; no named time sources |
| `setCommandTime` | refused: settings act at once |
| clock sources | `internal`, `rx`, `tx`: the `clkout` setting (`internal` is off) |
| time sources | `burst`, `trigger`, `none`: the `time_origin` setting |
| master and reference clock rates | refused |
| sensors | `lo_locked` per channel; `ref_locked` and `time_locked` for the device |
| settings | `bias_tx`, `clkout`, `time_origin`, `calibrate`, `tx_underrun_limit`; none per channel |
| frontend mapping, registers, GPIO, I2C, SPI, UART | none listed; writes refused |

## Threads

Per board, libhackrf runs one transfer thread that calls the handler. The
handlers only touch the rings. The SoapySDR client calls settings from
any thread (serialised per board) and reads or writes each stream from
one thread, as SoapySDR requires. Tuning one direction locks only that
direction's board; the two directions share no lock. Opening, closing,
activating and deactivating streams, and calibrating, are serialised on
one lock of their own, and the transmit writer shares one with the
calibration burst, which is written into the same stream. Calibration
runs on the thread that activated the second stream, before
`activateStream` returns; a receive reader already running is not
disturbed, since the burst is heard through a tap beside it.

## Time

Time is counted on the receiving board's sample clock. The hardware time
is the number of samples the receiver has delivered since its stream
started plus an offset `setHardwareTime` sets, converted at the receive
rate; each receive read is stamped with its first sample's position plus
the same offset. It is the board's clock, not the host's, and honest
across drops, since a dropped transfer still advances the position.

The transmit stream counts positions the same way: every sample handed
to the board, silence included. What ties the two together is the
origin, one number: the transmit position on the air at the instant of
receive position 0. A transmit time T then goes out at transmit position
`rx_position(T) + origin`. The origin exists once both streams are
running and is found, according to the `time_origin` setting, in one of
two ways:

- `burst` (the default): when the second stream starts, the pair sends
  a calibration burst and listens for it. The burst is 4096 chips of a
  13-bit maximal-length sequence, one chip per sample at full scale on
  I and Q, taken from the point in the sequence where its aperiodic
  autocorrelation's sidelobes are lowest (32 dB under the peak). It is
  written to the transmit stream as a timed block at a known position,
  a tap beside the receive reader captures enough of the receive stream
  to hold it however full the transmit ring was, and an overlap-save
  correlation over a hand-written FFT finds it; the capture's mean is
  removed first, because the receiver's DC offset can be many times the
  burst. The correlation runs block by block as the capture fills
  (`burst::Finder`), each block taking the mean of the capture as it
  then stood, and the listen ends once a peak clears the threshold with
  a burst's length of correlation past it, so activation waits for the
  burst rather than for the worst case (about 27 transfers, 1.8 s at
  1.92 Msps). A peak 20 dB over the median correlation is accepted; anything
  less leaves no origin and logs the level found. If the two directions
  are tuned apart, the transmitter is retuned to the receive frequency
  for the burst and back afterwards; tuned alike it is not touched,
  since the firmware skips a tune to the frequency it last applied. The
  burst needs both directions at one sample rate and the boards on one
  reference: free-running, the crystals' offset turns the chips over
  within the burst. So it is not sent unless the boards share one: the
  fed board started on CLKIN while the other drives CLKOUT, or both
  started on CLKIN, taken to be one source. Each board's CLKIN and
  CLKOUT are read while it is idle at its activation, because the clock
  chip reads as zero while a board streams (on the older boards even the
  CLKIN status is a clock-chip register), and the firmware chooses its
  reference at the start anyway; CLKOUT changes through the pair are
  followed. Refused, the log says why and names `clkout`.
- `trigger`: the first stream's activation only arms it; the second
  starts the transmitting board in hardware sync mode and then the
  receiving board, whose start drives its trigger output into the
  transmitter's trigger input through a jumper (P28 pin 15 to pin 16).
  Both start on one edge, so the origin is 0. If the transmitter has not
  moved two transfers within 500 ms, no edge arrived: both stop and the
  error names the jumper. Built and tested against the fake board; not
  yet tried on the bench.
- `none`: no origin.

A timed write places its first sample at its position, and the board's
transfer thread, not the writer, does the placing: it sends silence
until the block's position comes round, which is not an underflow, and
drops a block it reaches too late, with the rest of its burst. Placement
belongs there because only that thread knows where silence it inserted
into an empty ring has moved later samples. A write whose position is
already queued or sent is refused at once with `SOAPY_SDR_TIME_ERROR`.
A timed write while the origin is still to come waits for it up to its
timeout; with no origin to come it is refused.

Receive is timed at activation. `activateStream` with
`SOAPY_SDR_HAS_TIME` gives the receive path a window starting at that
time's position: the reader drops every earlier sample, gaps included
since the positions say where the window falls, and its first read is
stamped exactly at the time asked. With `SOAPY_SDR_END_BURST` and a
count the window delivers that many samples, flags the last read, and
then drops what follows until the next activation. Either works on a
stream already running, which is how a receive window is laid over a
timed transmit burst without stopping the streams and losing the
origin; a plain `activateStream` on a running stream returns it to
delivering everything. A time before the hardware time now is refused
with `SOAPY_SDR_TIME_ERROR`.

The origin is lost, reported once through `readStreamStatus` as
`SOAPY_SDR_TIME_ERROR` and logged, when either board reports a
shortfall (Failure handling, below) or a sample rate changes while
streaming. A shortfall found while the burst is out leaves no origin. Stopping
either stream leaves it to be found again when both run. Writing the
`calibrate` setting runs the burst again at any time. `hasHardwareTime`
is true while an origin holds.

## Latency

libhackrf moves 256 KiB per USB transfer, four transfers deep, and the
firmware itself moves 16 KiB at a time. Per direction that is:

| Sample rate | 16 KiB firmware chunk | 256 KiB libhackrf transfer |
|---|---|---|
| 0.2 Msps | 41 ms | 655 ms |
| 1.2 Msps | 6.8 ms | 109 ms |
| 2 Msps | 4.1 ms | 65.5 ms |
| 8 Msps | 1.0 ms | 16.4 ms |
| 10 Msps | 0.8 ms | 13.1 ms |
| 20 Msps | 0.4 ms | 6.6 ms |

The ring adds up to `buffers` transfers of latency on the transmit side
only when the writer runs ahead; the receive side's ring is slack, not
delay. An application that needs a short loop between what it receives
and what it transmits is bounded by the transfer size, which is a
compile-time constant of libhackrf. The module uses libhackrf as it is
built; shrinking the transfers is a libhackrf change, outside this
project.

## Measured

The figures come from two host classes, named with each table: an Intel
Mac with the boards on USB 2 hubs shared with other devices (the
workstation, until 2026-11-09), and a Raspberry Pi 5 on Debian trixie
with each board on its own root port (the target, from 2026-11-09).
Every section after "On the target" was measured on the workstation.

### On the target

A Raspberry Pi 5, libhackrf 2024.02.1, both boards on firmware
2026.01.3, `clkout=rx`, 868.3 MHz, TX gain 0 with the amplifier off, the
boards stacked with stub antennas; `vcgencmd get_throttled` read the
same before and after every run, with no under-voltage during any.

| Check | Result |
|---|---|
| Receive only, 2 and 8 Msps, 10 s each | 99.9% of nominal, 0 overflows, both boards `clock: external` |
| Reference switch, the TX board onto its crystal (CLKOUT off), twice | switch logged and handled, `lo_locked` true, tone 12.1 kHz low (the crystal) |
| Reference switch, the TX board back onto CLKIN (CLKOUT on), twice | switch logged and handled, `lo_locked` true, tone at +0.0 Hz |
| Full duplex, `--tx-tone 100e3 --seconds 10` | origin found (36.8 dB over the median), tone -44 dBFS at +0.0 Hz every second, image 31 dB down, 0 underflows, 1 overflow |
| `--timed-bursts 81920 --seconds 60` | 3658 of 3658 bursts at +0 samples, 0 late writes, 0 time errors, 0 underflows, hardware time inside its reads 3662 of 3662 |

With transmit, the receive rate reads 102 to 108% of nominal and there is
one overflow: the receive ring fills while activation calibrates, and
the check tool's clock starts after that, so the backlog is counted
within the run. That is the check tool's accounting, not the rate.
20 Msps receive through the check tool reaches only 81% here, with USB
alone carrying the full rate (ACTIVE item 11).

### On the workstation

Receive only, `hackrfduplex-check --seconds 10`, one HackRF One r10
(firmware 2024.02.1) on a USB 2 bus shared with two other devices, an
Intel Mac host:

| Rate | Achieved | Overflows in 10 s |
|---|---|---|
| 2 Msps | 100.0% | 0 |
| 8 Msps | 100.0% | 0 |
| 20 Msps | 99.9% | 0 |

The stream time spanned the elapsed time to within the last transfer in
each run.

Full duplex, `hackrfduplex-check --clkout rx --tx-tone 100e3 --tx-gain 0
--seconds 10`: both streams at 8 Msps, 868.3 MHz, the transmitter at
0 dB with its amplifier off, stub antennas 20 cm apart, the two boards on
one reference through a CLKOUT to CLKIN cable:

| Measure | Result |
|---|---|
| Receive rate, overflows | 99.9% of nominal, 0 |
| Transmit underflows | 0 |
| Tone level at the receiver | -77 dBFS, 9 dB above the median 977 Hz bin |
| Tone offset from where it was sent | 0.0 Hz, every second for 9 s (1 Hz resolution) |

The transmit side reports about 2.5% more samples than nominal because
the writer fills the ring ahead of the board at the start; that is
buffering, not rate.

With the reference shared, the tone arrives at exactly the transmitted
offset and does not move, which is the lock working: a free-running pair
differs by up to the crystals' tolerance, about 17 kHz at 868 MHz for
20 ppm.

The strongest signal in the band, with or without the transmitter, is at
870.000 MHz (+1.7 MHz from centre), the 87th harmonic of the 10 MHz
reference. It is 8 dB stronger while CLKOUT drives the cable, so the
cable radiates the reference. A shorter cable took it from -64.3 to
-69.3 dBFS, within 3.3 dB of its level with CLKOUT off; what remains
comes mostly from the boards' own references. A centre frequency away
from multiples of 10 MHz avoids it entirely.

### Time origin and timed transmit

The calibration burst at 8 Msps, 868.3 MHz, TX gain 0, amplifier off,
the boards on one reference, `hackrfduplex-check --tx-tone 100e3
--calibrations 2`:

| Measure | Tuned alike | TX 45 MHz below RX |
|---|---|---|
| Starts with an origin found | 10 of 10 | 10 of 10 |
| Correlation peak over the median | 27.8 to 30.2 dB | 27.5 to 30.1 dB |
| Spread of the origin within one start, 3 findings each | 0 samples every start | 0 samples every start |
| Spread within one start, 10 findings | 0 samples | 0 samples |

The origin itself differs from start to start, by 0.1 to 1.6 ms here,
since it holds how far apart the host started the two streams. Before
the capture's mean was removed the same burst, then at 0.7 of full scale
on I only, stood 12.4 dB over the median, under the receiver's DC
offset.

Timed bursts, `hackrfduplex-check --timed-bursts 81920 --seconds 60`:
the 4096-chip sequence written for each live receive timestamp plus
81.92 ms (five transfers at 8 Msps) and found again by correlation:
3658 of 3658 bursts landed at +0 samples from the time asked, with no
late write, no time error and no underflow, and the hardware time read
between two reads lay between their timestamps all 3662 times. The
check tool does not stamp bursts from reads out of a backlog: the
receive ring fills while activation calibrates, and its first 15 reads
are old.

### Low sample rates

The HackRF documentation recommends 8 Msps and up, since the MAX5864
converter is not specified below that and the narrowest baseband filter
is 1.75 MHz. Below 2 Msps, measured with `hackrfduplex-check --tx-tone
40e3 --tx-gain 0 --seconds 10`, each board in each role, the other
direction held at a rate it handles, both boards on one reference:

| Direction | Rate | Result on both boards |
|---|---|---|
| both | 1.92, 1.6, 1.2 Msps | tone at its sent offset, 0.0 Hz every second, 0 overflows and underflows |
| receive | 1.0, 0.8, 0.4, 0.2 Msps | tone at its sent offset every second, 0 overflows; image rejection 2 to 6 dB worse than at 8 Msps |
| transmit | 1.1, 1.08 Msps | tone at its sent offset, image 16 dB down |
| transmit | 1.05, 1.0 Msps and below | streams without an underflow but one board puts out nothing and the other only one of I and Q (tone and image level) |

Below 1.05 Msps the transmitters fail while the sample clock is right and
the stream is healthy, so the limit is in the converter. Receive is
limited by the clock chip: the firmware divides an 800 MHz PLL by at
most 2048 to make twice the sample rate, about 195 ksps at the
slowest. The module advertises 0.2 to 20 Msps for receive and 1.2 to
20 Msps for transmit, the transmit floor keeping a margin above the
failure, and passes any other rate through untouched.

At these rates one transfer takes long: 655 ms at 200 ksps, 109 ms at
1.2 Msps. A read or write timeout shorter than that returns
`SOAPY_SDR_TIMEOUT` between transfers.

### Reference switches

The firmware re-chooses its clock source every time a stream starts
(`activate_best_clock_source`, called from `transceiver_startup`): the
CLKIN reference if the Si5351 sees one, otherwise the board's own
crystal. It does so after tuning, so when the reference actually changes
the RF synthesisers are left locked to nothing: a board receiving goes
deaf and a board transmitting puts out no signal at all, while its
sample clock, already on the new reference, streams at the full rate as
if nothing were wrong. Retuning cures it, but the firmware also skips a
tune to the frequency it last applied (`applied_freq` in `radio.c`).

So `Pair::activate` asks the board, while still idle, whether this
start will switch its reference: the CLKIN status the firmware is about
to act on, against what the clock chip runs on now (CLK0's PLL source
bit, or for boards with id `HACKRF1_R9` the PLL A multiplier). Clock
chip registers read as zero while a board streams, which is why the
question is asked before the start rather than answered after it. If it
will switch, the board is started, tuned 1 MHz away and back, stopped
and started again. If not, nothing extra happens: retuning a running
board without a switch left one board's receiver 1.7 kHz off until it
was reset. The `lo_locked` sensor reads false from the moment a switch
is detected until the restart is done, and stays false if handling it
fails.

The firmware chooses its clocking scheme by board id, not by revision.
One of the boards here reports revision r10 but id `HACKRF1_OG`, as
clones do, and is clocked the older way.

Measured with the switch handled, both boards on one reference unless
free-running:

| On the crystal board | Reference | Result |
|---|---|---|
| Transmit, 4 runs | shared | tone at the sent offset, 0.0 Hz every run |
| Transmit, 3 runs | own crystal | tone 13.1 kHz low every run |
| Receive, 2 runs | shared | outside carriers at their true offset |
| Receive, 2 runs | own crystal | the same carriers 13.7 kHz high |

The two crystal figures are the same error, about 15 ppm slow at
868-892 MHz, seen once from each side.

## Failure handling

A libhackrf error throws `BoardError` from the call that made it, which
SoapySDR turns into an exception for the client. Stream activation
catches it and returns `SOAPY_SDR_STREAM_ERROR` with the message logged,
as the SoapySDR stream API expects. A read with nothing to deliver is a
timeout, not an error; a read or write on a stream that is not active
returns `SOAPY_SDR_STREAM_ERROR`.

While a stream is active its board is watched: at most every 100 ms,
from the reads, writes and status calls, the pair reads each running
board's shortfall count and whether its transfers still run. A blocked
read or write waits in slices of that length, so it looks again however
long its timeout.

- A shortfall is the board's own buffer running full (receiving) or
  empty (transmitting): samples lost or delayed where the stream
  positions cannot see. On the receiver the next read returns
  `SOAPY_SDR_OVERFLOW` with `SOAPY_SDR_END_ABRUPT`, once, and the log
  says the timestamps from there on are shifted by an unknown amount; on
  the transmitter `readStreamStatus` returns `SOAPY_SDR_UNDERFLOW` once.
  Either loses the origin. Each poll that finds new shortfalls reports
  them to the stream so, but the log names the first at once and the
  rest at most once a second, with their count, and any left when the
  stream stops: at 20 Msps on the target a starved stream falls short
  at every poll. The firmware counts shortfalls from each start, and so
  does the pair.
- A board whose transfers have stopped, on which a libhackrf call fails
  (as on one pulled from the bus), or whose shortfall count goes back,
  which only a start under the stream does, has failed: the error is logged
  once, and its stream's reads, writes and status calls return
  `SOAPY_SDR_STREAM_ERROR` until it is deactivated and activated again.
  Deactivating it ends the stream even when the board cannot be told to
  stop, and passes that error on.
- The transmitter cannot outlive its host. Before every transmit start
  the pair sets the board's underrun limit (`tx_underrun_limit`, one
  transfer's worth unless set; 0 turns it off): once the board goes that
  long without data in one stretch, its M0 drops to idle on its own and
  records a timeout. The pair never leaves it dry while it runs, since
  the transmit path pads every transfer, so the limit trips only when
  the host has stopped feeding it: the process killed, or USB gone. A
  board that stopped itself so has failed, as above. The firmware keeps
  the limit across opens, so a clean stop sets it back to 0 and leaves
  the board as it was found; a killed process cannot, and the next
  program to transmit on that board inherits the limit.
