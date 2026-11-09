# hackrfduplex

Version 0.1.0

A [SoapySDR](https://github.com/pothosware/SoapySDR/wiki) module that
bonds two HackRF One boards into one full-duplex radio. One board only
ever receives and the other only ever transmits, so a SoapySDR
application sees a single device with an RX channel and a TX channel
that run at the same time, each tuned, filtered and gained on its own.
The driver key is `hackrfduplex`.

The HackRF One is a half-duplex radio. Two of them, with this module, are
the cheapest full-duplex SoapySDR device there is. The module works with
stock HackRF firmware and stock libhackrf.

The code is C++20, tested with Catch2. The dependencies are SoapySDR,
libhackrf and Catch2.

Documentation:

| Document | Contents |
|---|---|
| [docs/design.md](docs/design.md) | how the module is built: the pair, the stream paths, threads, timestamps, latency, reference switches, measurements |
| [docs/licensing.md](docs/licensing.md) | licence and third-party works |
| [CONTRIBUTING.md](CONTRIBUTING.md) | repository layout, conventions, transmit safety |
| [TODO/ACTIVE.md](TODO/ACTIVE.md) | open work, in order |

## Status

| Component | Working | Notes |
|---|---|---|
| Discovery and opening a pair | ✓ | two boards pair themselves; more need `rx_serial` and `tx_serial` |
| Receive stream | ✓ | CS8, CS16, CF32, CF64; a stream time from the board's sample count; overflows reported |
| Transmit stream | ✓ | the same formats; end of burst; underflows reported through the stream status |
| Tuning, filter, gains, bias tee | ✓ | per direction; an overall gain is split over the stages |
| `hackrfduplex-check` | ✓ | receives, optionally transmits a tone, and reports rates, drops and levels |
| Receive verified on two boards | ✓ | 2, 8 and 20 Msps for 10 s without an overflow |
| Transmit verified on two boards | ✓ | a tone in each of CS8, CS16, CF32 and CF64 received at exactly its sent offset, levels within 0.3 dB, 0 underflows |
| Shared reference clock | ✓ | the `clkout` setting; one TCXO board drives both through a CLKOUT to CLKIN cable; a board whose reference changes at a stream start is retuned and restarted, so it can be switched freely between runs |
| Hardware time: synchronised start, timed transmit | ✗ | planned; needs the boards' trigger pins wired together ([Roadmap](#roadmap)) |

## Hardware

Two HackRF Ones on USB, ideally on separate host controllers: each moves
up to 40 MB/s and the HackRF is a USB 2 device. `hackrf_info` warns when
a board shares its bus.

Between the two boards, either antennas with enough separation or
attenuation for the receiver not to be overloaded, or an SMA cable with
an attenuator of at least 30 dB. The transmitter puts out up to about
+10 dBm with its amplifier on and the receiver's front end saturates far
below that.

Optional, for applications that need the two sample clocks and both
local oscillators locked: an SMA cable from one board's CLKOUT to the
other's CLKIN, and the `clkout` setting naming the source board. The fed
board then reports `clock: external` in the hardware information and
the source `clkout: on`. A board with a TCXO module on its CLKIN header
cannot also be fed by cable; take the module off the fed board.

## Build and install

macOS with Homebrew:

```
brew install cmake hackrf soapysdr catch2
```

Debian and Ubuntu:

```
sudo apt install build-essential cmake pkg-config libhackrf-dev libsoapysdr-dev catch2
```

Then, from the repository root:

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
sudo cmake --install build
```

`cmake --install` places the module in SoapySDR's modules directory
under the install prefix and `hackrfduplex-check` in `bin`. To use the
module from the build tree instead, point SoapySDR at it:

```
export SOAPY_SDR_PLUGIN_PATH=$PWD/build/module
SoapySDRUtil --find="driver=hackrfduplex"
```

## Use

With exactly two boards attached the pair chooses itself, the lower
serial receiving. With more, or to pick the roles, name the boards by
the end of their serials:

```
SoapySDRUtil --probe="driver=hackrfduplex,rx_serial=244b,tx_serial=7957"
```

| Key | Default | Meaning |
|---|---|---|
| **Device arguments** | | |
| `rx_serial` | the lower of two serials | suffix of the serial of the board that receives |
| `tx_serial` | the higher of two serials | suffix of the serial of the board that transmits; with only one of the two given, the other attached board takes the other role |
| **Stream arguments** | | |
| `buffers` | 15 | USB transfers (256 KiB, 128 Ki samples each) the stream holds between the board and the caller |
| **Settings** | | |
| `bias_tx` | false | 3.3 V on the transmitting board's antenna port while it transmits |
| `clkout` | off | `rx` or `tx`: that board drives 10 MHz from its CLKOUT for the other's CLKIN, so both run from one reference; off at every power-up, so set it each run |

Gains, per direction: RX `LNA` (0 to 40 dB in 8 dB steps), `VGA` (0 to
62 dB in 2 dB steps) and `AMP` (0 or 14 dB); TX `VGA` (0 to 47 dB) and
`AMP` (0 or 14 dB). Setting one overall gain spreads it over the stages
and switches the amplifier on only when the others cannot reach it.
Sample rates 2 to 20 Msps; the baseband filter follows the rate at three
quarters of it until set, and a set bandwidth rounds down to the nearest
of the MAX2837's filters.

The receive stream carries a time, counted in samples from the moment the
stream started, so a gap left by a dropped transfer shows in the
timestamps as well as in the overflow return. The transmit stream ignores
timestamps for now.

`hackrfduplex-check` exercises the whole path as a SoapySDR client. It
receives by default and transmits nothing:

```
hackrfduplex-check --seconds 5
hackrfduplex-check --tx-tone 100e3 --tx-gain 0 --seconds 5
```

With `--tx-tone` it transmits a tone that far from the TX centre and
reports its level at the receiver, which with two antennas or a cable
confirms both halves of the pair in one run.

## Build options

| Option | Effect |
|---|---|
| `-DHACKRFDUPLEX_BUILD_MODULE=OFF` | skip the SoapySDR module (and the need for SoapySDR) |
| `-DHACKRFDUPLEX_BUILD_TESTS=OFF` | skip the unit tests (and the need for Catch2) |
| `-DHACKRFDUPLEX_BUILD_TOOLS=OFF` | skip `hackrfduplex-check` |
| `-DHACKRFDUPLEX_SANITIZE=ON` | AddressSanitizer and UndefinedBehaviorSanitizer (`cmake --preset asan`) |
| `-DHACKRFDUPLEX_COVERAGE=ON` | gcov instrumentation with a `coverage` target (`cmake --preset coverage`) |

## Roadmap

The module does one thing: a pair of HackRF Ones as one full-duplex
SoapySDR device. Open work is in [TODO/ACTIVE.md](TODO/ACTIVE.md):

1. Debian packaging and a build-and-test script.
2. Sample rates below 2 Msps, measured and advertised.
3. A hardware time: both streams started on one edge of the boards'
   trigger pins, receive timestamps and timed transmit on that origin,
   with late transmissions reported, and the shortest transmit lead the
   pair can honour measured.
4. An `lo_locked` sensor, and unknown settings ignored rather than
   refused.

Applications built on the pair, and changes to the HackRF firmware or
libhackrf, are out of scope.

## AI

This project was developed with the use of agentic engineering,
specifically Claude.

## Licensing

Licensed under the GNU General Public License, version 3 or (at your
option) any later version; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Third-party works and their licences are listed in
[docs/licensing.md](docs/licensing.md).
