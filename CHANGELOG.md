# Changelog

## 0.2.1 - 2026-11-12

- Fixed: the calibration burst is queued eight transfers ahead of the
  transmitter, not one, so a host stall while the streams start no longer
  loses the time origin.
- Fixed: the packages build-depend on `libusb-1.0-0-dev`, which Debian's
  `libhackrf-dev` needs but does not pull in.
- Packages for amd64 as well as arm64, in the tomcully.com apt repository.
- Tests keep their timing margins milliseconds wide on a busy host.

## 0.2.0 - 2026-11-12

- Faults reach the application as stream statuses: lost samples, a
  stopped or unplugged board (within 0.1 s), a lost time origin.
- The transmitter stops by itself when its host stops feeding it
  (`tx_underrun_limit`).
- Timed receive windows: `activateStream` on receive takes a start time
  and a sample count.
- Clock and time sources (`internal`, `rx`, `tx`; `burst`, `trigger`,
  `none`), and the `ref_locked` and `time_locked` sensors.
- Calls the device cannot honour are refused, not ignored; so are sample
  rates outside each direction's range.
- SoapySDR direct buffer access on a CS8 receive stream.
- A receive ring of 1.5 s at the rate; the device opens at 10 Msps; idle
  reads time out; SoapyRemote's keys pass.
- Optional receive DC removal and I/Q balance correction
  (`-DHACKRFDUPLEX_DSP=ON`, liquid-dsp).
- Format conversions built at -O3.
- `hackrfduplex-check`: any receive format, direct access, CPU use,
  `--recover`, the hardware acceptance battery, and a man page.
- `hackrfduplex-ft8`, an FT8 decoder example.
- Checked with GNU Radio, SoapyRemote, rtl_433 and the Python bindings.
- CI on GitHub Actions: sanitizers, thread sanitizer, Clang's
  thread-safety analysis, clang-tidy, coverage and CodeQL.
- Fixed: the transmit frequency after the calibration burst; a start
  that read the previous stream's board state; a failed reopen handing
  libhackrf a null device.

## 0.1.0 - 2026-11-09

The rewrite as hackrfduplex; packaged, not tagged.

- Two HackRF Ones as one SoapySDR device, one receiving and one
  transmitting, each direction tuned and set on its own.
- Streams in CS8, CS16, CF32 and CF64; frequency, gains, filters and bias
  tee per direction.
- A shared reference (`clkout`), with boards retuned when their reference
  changes at stream start; an `lo_locked` sensor per channel.
- A common time origin from a calibration burst or a trigger edge, the
  hardware time API, and timed transmit to the sample.
- Sample rates from 0.2 Msps receive and 1.2 Msps transmit.
- `hackrfduplex-check`, with a whole-band spectrum, a tone search and
  timed bursts.
- Catch2 tests against simulated boards, `tools/ci.sh`, and Debian
  packaging.
