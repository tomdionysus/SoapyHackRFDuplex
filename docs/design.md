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
transmit stream has no time yet. Giving it one needs an origin shared
with the receive stream, which the hardware can supply through its
trigger pins; that is the roadmap's second item.

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
compile-time constant of libhackrf; the roadmap's fourth item is to
measure and then shrink it.

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
each run. Transmit figures follow when that test has run.

## Failure handling

A libhackrf error throws `BoardError` from the call that made it, which
SoapySDR turns into an exception for the client. Stream activation
catches it and returns `SOAPY_SDR_STREAM_ERROR` with the message logged,
as the SoapySDR stream API expects. A read with nothing to deliver is a
timeout, not an error; a read or write on a stream that is not active
returns `SOAPY_SDR_STREAM_ERROR`.
