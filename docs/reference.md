# Reference

Every argument, setting, range and sensor the device offers. How to use
them together is in [usage.md](usage.md); how they are built is in
[design.md](design.md).

## Choosing the boards

With exactly two HackRF Ones attached the pair chooses itself, the lower
serial receiving. With more, or to choose the roles, name the boards by
the end of their serials:

```
SoapySDRUtil --probe="driver=hackrfduplex,rx_serial=244b,tx_serial=7957"
```

Discovery lists both orderings of two boards; device 0 is the default.

## Arguments and settings

| Key | Default | Meaning |
|---|---|---|
| **Device arguments** | | |
| `rx_serial` | the lower of two serials | end of the serial of the board that receives |
| `tx_serial` | the higher of two serials | end of the serial of the board that transmits; with only one of the two given, the other attached board takes the other role |
| `bias_tx`, `clkout`, `time_origin`, `tx_underrun_limit` | as the settings | the settings of the same names, applied when the device opens |
| **Stream arguments** | | |
| `buffers` | receive: 1.5 s at the rate, at least 15; transmit: 15 | USB transfers (128 Ki samples each) the stream holds between the board and the caller; unset on receive, the module sizes the ring from the sample rate at each activation |
| **Settings** | | |
| `bias_tx` | false | 3.3 V on the transmitting board's antenna port while it transmits |
| `clkout` | off | `rx` or `tx`: that board drives 10 MHz from its CLKOUT into the other's CLKIN, so both run from one reference; off after every power-up, so set it on every run; also the SoapySDR clock source, `internal` for off |
| `time_origin` | `burst` | how the two streams get one time once both have started: `burst` sends a short calibration burst on the receive frequency; `trigger` starts both boards on one edge through a jumper from the receiving board's P28 pin 15 to the transmitting board's P28 pin 16; `none` gives no shared time; takes effect when the streams next start; also the SoapySDR time source |
| `calibrate` | | write any value to send the calibration burst again now; both streams must be running |
| `tx_underrun_limit` | 131072 | samples: the transmitting board stops by itself after this long without data, so it cannot transmit on after its host dies; 0 turns it off |

Unknown keys are tolerated, since applications pass whole argument
strings around and write vendor settings blindly: each is logged as a
warning and ignored, and reading an unknown setting returns an empty
string. SoapyRemote's own keys (`remote`, `remote:...`,
`soapy_remote_...`) are ignored without a warning. Anything else the
device cannot do is refused, never ignored: a stream flag it cannot
honour returns `SOAPY_SDR_NOT_SUPPORTED`, and any other call throws,
naming why.

## Rates, filters and gains

| | Receive | Transmit |
|---|---|---|
| Sample rate | 0.2 to 20 Msps | 1.2 to 20 Msps |
| Rate at open | 10 Msps | 10 Msps |
| Gain stages | `LNA` 0 to 40 dB (8 dB steps), `VGA` 0 to 62 dB (2 dB steps), `AMP` 0 or 14 dB | `VGA` 0 to 47 dB, `AMP` 0 or 14 dB |
| Formats | CS8 (native), CS16, CF32, CF64 | the same |

A rate outside its direction's range is refused, and the rate already
set is kept. 8 Msps and up give the cleanest spectrum. The host's USB
sets the practical top: on a Raspberry Pi 5, full duplex is clean
through 16 Msps.

The baseband filter follows the rate, at three quarters of it rounded
down to one of the MAX2837's filters (1.75 to 28 MHz), until a bandwidth
is set; a set bandwidth rounds down the same way.

An overall gain is spread over the stages, with the amplifier switched
on only when the others cannot reach it.

The receive channel can remove its DC offset and correct its I/Q
balance (`setDCOffsetMode`, `setDCOffset`, `setIQBalanceMode`,
`setIQBalance`) in a module built with `-DHACKRFDUPLEX_DSP=ON`
([building.md](building.md)).

## Sensors

| Sensor | Where | True when |
|---|---|---|
| `lo_locked` | each channel | the synthesisers are tuned on the reference the board runs from; false only while a board whose reference changed at stream start is retuned |
| `ref_locked` | device | the boards run from one reference: one board's CLKOUT into the other's CLKIN with `clkout` set to it, or both on CLKIN |
| `time_locked` | device | the streams share a time origin, as `hasHardwareTime()` is |

libhackrf cannot report synthesiser lock, so `lo_locked` is the module's
own account.

## Time

The receive stream counts samples from the moment it starts, so a gap
from lost samples shows in the timestamps as well as in the overflow
status. `getHardwareTime` reads that clock and `setHardwareTime` moves
it.

Once both streams run the module ties the transmit count to the receive
count (the time origin, [usage.md](usage.md)). A write with
`SOAPY_SDR_HAS_TIME` at time T is then received at T. A finite receive,
`activateStream(rx, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST, timeNs,
count)`, delivers exactly `count` samples from that time.

Each status a stream call can return, and what to do about it, is in
[usage.md](usage.md#stream-statuses).

## hackrfduplex-check

`hackrfduplex-check` exercises the whole path as a SoapySDR client and
reports what it found. It receives only, unless told to transmit:

```
hackrfduplex-check --clkout rx --seconds 5
hackrfduplex-check --clkout rx --tx-tone 100e3 --tx-gain 0
hackrfduplex-check --clkout rx --timed-bursts 90112 --seconds 60
hackrfduplex-check --clkout rx --acceptance --transmit
```

The first receives and reports the rate, losses, CPU, versions and
sensors. The second transmits a tone and finds it at the receiver,
checking both boards in one run. The third transmits a burst a lead
after each receive timestamp and reports where each arrived. The last
runs the hardware acceptance battery, printing PASS or FAIL per line.
`hackrfduplex-check --help` and `man hackrfduplex-check` list every
option.
