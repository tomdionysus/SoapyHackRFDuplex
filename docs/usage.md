# Using the pair

A guide for writing an application against the pair: setting it up,
what the shared time is and what it costs, how far ahead to transmit,
what each stream status means, and what to do when something goes
wrong. The README lists every argument and setting; docs/design.md says
how the module is built and holds the measurements quoted here.

## Setting up

**Two boards.** Any two HackRF Ones on firmware with USB API 1.06 or
later (the transmitter then stops by itself if its host dies). With
exactly two attached the pair chooses itself, the lower serial
receiving; name them otherwise (`rx_serial`, `tx_serial`, the end of
each serial is enough). Put the board with the better receive image
rejection on receive: on older boards it can be as poor as 9 dB.

**USB.** Each board moves up to 40 MB/s at 20 Msps, and the HackRF is a
USB 2 device, so give each its own host controller where the host has
two; `hackrf_info` warns when two boards share a bus. On a Raspberry
Pi 5 full duplex is clean through 16 Msps; at 20 Msps neither board
keeps up there, with this module or with `hackrf_transfer`.

**Power.** Two HackRFs draw more than a Pi 5 gives its USB ports
without the 5 A supply (600 mA in all). An under-powered host drops
samples, which reach the application as overflows and underflows, or
stops a board. On a Pi, `vcgencmd get_throttled` reports under-voltage;
read it when a run goes wrong before suspecting anything else.

**The RF path.** The boards must hear each other for the time origin
(below), and must not overload each other. Two antennas close together
at TX gain 0 are plenty; a cable needs an attenuator of at least 30 dB.
The transmitter's amplifier is off unless an application turns it on.

**The reference.** For the two sample clocks and both local oscillators
to agree, one board drives 10 MHz from its CLKOUT into the other's
CLKIN over an SMA cable, and the `clkout` setting names the driving
board (`clkout=rx` or `clkout=tx`, as a device argument or setting). A
CLKOUT is off after every power-up, so set it on every run. A board with
a TCXO module on its CLKIN header cannot also take the cable: take the
module off the board that is fed. The `ref_locked` sensor is true when
the boards run from one reference. A board whose reference changes when
its stream starts is retuned and restarted on the new one, so the
reference can be switched between runs; its `lo_locked` sensor is false
meanwhile.

Check the setup with:

```
SoapySDRUtil --probe="driver=hackrfduplex,clkout=rx"
hackrfduplex-check --clkout rx --seconds 5
```

## The time origin

Each board counts its own samples from its own start, so the receive
and transmit sample counts begin with an unknown offset. Once both
streams are running the module measures that offset, and from then on
the device has one time: `hasHardwareTime()` is true, the `time_locked`
sensor is true, and a write with `SOAPY_SDR_HAS_TIME` at time T goes out
so that it is received at T.

By default (`time_origin=burst`) it measures the offset by
transmitting. Inside the second `activateStream`, whichever stream that
is, the transmitter tunes to the receive frequency if it is elsewhere,
sends a 4096-sample calibration burst at the transmit gain set, and
tunes back; the module
finds the burst in what it receives. So:

- The pair transmits briefly on the receive frequency at every start of
  both streams, and on `writeSetting("calibrate", "")`. Choose the
  transmit gain with that in mind. `time_origin=none` sends nothing and
  gives no shared time.
- The second activation takes a moment longer: about 0.25 to 0.4 s.
  Nothing reads the receive stream meanwhile unless the application has
  a reader running. The receive ring holds half a second at the rate by
  default, so it rides that out without an overflow; a ring the
  application made smaller may overflow once, at the start.
- Calibration needs both directions at one sample rate, the boards on
  one reference, and the burst to reach the receiver at the gains set.
  When it fails the module logs why, the streams run on, and
  `hasHardwareTime()` stays false.

`time_origin=trigger` starts both boards on one edge instead, through a
jumper from the receiving board's P28 pin 15 to the transmitting
board's P28 pin 16. It transmits nothing. It has been tested against
the fake boards but not yet on hardware.

The origin is lost, and reported (below), when a board drops samples on
its own, or when a sample rate changes while the streams run. Write
`calibrate` to find it again; deactivating and activating the streams
finds a new one too.

## Transmitting at a time

A timed write must reach the transmitter before its samples are due.
Between a write and the antenna lie the module's ring, libhackrf's four
transfers in flight and the firmware's buffer, so a burst timed from a
receive timestamp needs a lead: allow 5.5 USB transfers, 720 896
samples, at any rate (360 ms at 2 Msps, 90 ms at 8 Msps, 45 ms at 16
Msps). Measured on the target, nothing was late from 4.5 to 5
transfers; the rest is margin.

Count the lead from a read the reader waited for. A read of samples
already queued is older than it looks, and so is the read that empties
the queue: its samples arrived while the older ones were being read.
After an activation, after an overflow, or whenever the application has
fallen behind, read until two reads in a row end within a buffer of
`getHardwareTime()`, and take the time from the second.

In Python, the pattern `tools/timed_loopback.py` uses:

```python
rate = 8e6
lead = int(5.5 * 131072)
caught_up = False
while True:
    r = device.readStream(rx, [buffer], len(buffer), timeoutUs=1_000_000)
    tick = SoapySDR.timeNsToTicks(r.timeNs, rate)
    newest = SoapySDR.timeNsToTicks(device.getHardwareTime(), rate)
    was_caught_up, caught_up = caught_up, newest - (tick + r.ret) < len(buffer)
    if caught_up and was_caught_up:
        break
at = tick + lead
device.writeStream(tx, [burst], len(burst),
                   flags=SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST,
                   timeNs=SoapySDR.ticksToTimeNs(at, rate))
```

End each burst with `SOAPY_SDR_END_BURST`: the transmitter then sends
silence until the next one without reporting an underflow. A write
without a time goes out after whatever is queued.

A finite receive works the other way round: `activateStream(rx,
SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST, timeNs, count)` delivers
exactly `count` samples from that time, the last read flagged
`SOAPY_SDR_END_BURST`.

## Stream statuses

Every fault reaches the application as a status, within about 100 ms,
never as silently wrong samples or times.

| Where | Status | Means | Do |
|---|---|---|---|
| `readStream` | `SOAPY_SDR_TIMEOUT` | nothing arrived within the timeout | read again |
| `readStream` | `SOAPY_SDR_OVERFLOW` with `SOAPY_SDR_END_ABRUPT` | samples were lost before the next read: the reader fell behind and the ring was full (the timestamps stay true and show the gap), or the board dropped them itself (a shortfall: the timestamps have shifted by an unknown amount and the origin is lost) | read on; if `hasHardwareTime()` is now false, write `calibrate` |
| `readStream` | `SOAPY_SDR_STREAM_ERROR` | the board stopped or left the bus | deactivate and activate the stream; reopen the device if it is gone |
| `writeStream` | `SOAPY_SDR_TIMEOUT` | the ring was full for the whole timeout, or a timed write found the origin still being measured | write again |
| `writeStream` | `SOAPY_SDR_TIME_ERROR` | a timed write for a time already gone, or with no origin | write later, or calibrate |
| `writeStream` | `SOAPY_SDR_STREAM_ERROR` | the stream is not active, or the board failed | as for reads |
| `readStreamStatus` (transmit) | `SOAPY_SDR_UNDERFLOW` | the transmitter ran dry inside a burst (no `END_BURST` yet), or dropped samples itself | write faster, or end bursts |
| `readStreamStatus` (transmit) | `SOAPY_SDR_TIME_ERROR` | once for each write refused as late, for a burst found late on its way out, and for a lost origin | see `hasHardwareTime()` |
| `readStreamStatus` (transmit) | `SOAPY_SDR_STREAM_ERROR` | the transmitting board failed | as for reads |

A flag or call the device cannot honour is refused
(`SOAPY_SDR_NOT_SUPPORTED`, or an exception naming why), never ignored.

## Troubleshooting

**No time origin** (`hasHardwareTime()` false after both streams start;
the log says why). Check, in order: `clkout` set to the board that has
the cable on its CLKOUT (`ref_locked` true); both directions at one
sample rate; the boards able to hear each other at the gains set (raise
the transmit gain a little, or lower the receive gain if the receiver
is overloaded); `time_origin` not `none`. Then write `calibrate`.

**A board goes deaf or silent after the reference changed.** The module
retunes a board whose reference switches at stream start, and logs
"changed reference clock; retuned and restarted". If a board still shows
a steady offset of kilohertz with the boards locked, reset it
(`hackrf_spiflash -d <serial> -R`).

**Overflows or underflows in a steady run.** First the host: the power
flags, both boards on one USB controller, the CPU (`hackrfduplex-check
--no-analysis` reports the module's own share). Then the reader: a
receive reader that stalls for longer than the ring holds overflows; a
larger `buffers` stream argument holds more, at the cost of memory.

**Low sample rates.** Receive is clean from 0.2 Msps and transmit from
1.2 Msps, the ranges `getSampleRateRange` reports; below them the
receiver degrades and the transmitter goes silent or loses a channel.
The module does not refuse a rate below the range, so keep to it. 8 Msps
and up give the cleanest spectrum.

**The DC spike at the centre.** Every HackRF receiver has one. Tune a
little off the signal, or build the module with the receive corrections
(`-DHACKRFDUPLEX_DSP=ON`) and turn on `setDCOffsetMode`.
