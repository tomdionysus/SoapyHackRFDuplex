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
`hackrfduplex_core`, and includes neither libhackrf nor SoapySDR. The
tests drive it through `FakeBoard`, which records settings and lets the
test deliver and pull transfers by hand.

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
when full or when the caller marks the end of a burst. The board's thread
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

## Threads

Per board, libhackrf runs one transfer thread that calls the handler. The
handlers only touch the rings. The SoapySDR client calls settings from
any thread (serialised per board) and reads or writes each stream from
one thread, as SoapySDR requires. Tuning one direction locks only that
direction's board; the two directions share no lock.

## Time

The receive stream carries a time: the sample position since activation,
converted at the sample rate, with `SOAPY_SDR_HAS_TIME` set. It is the
board's clock, not the host's, and it is honest across drops. The
transmit stream has no time. Giving it one needs an origin shared with
the receive stream, which the hardware can supply through its trigger
pins; that is optional work in [TODO/ACTIVE.md](../TODO/ACTIVE.md).

## Latency

libhackrf moves 256 KiB per USB transfer, four transfers deep, and the
firmware itself moves 16 KiB at a time. Per direction that is:

| Sample rate | 16 KiB firmware chunk | 256 KiB libhackrf transfer |
|---|---|---|
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
