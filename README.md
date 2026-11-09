# hackrfduplex

Version 0.1.0

A [SoapySDR](https://github.com/pothosware/SoapySDR/wiki) module that
bonds two HackRF One boards into one full-duplex radio. One board only
ever receives and the other only ever transmits, so a SoapySDR
application sees a single device with an RX channel and a TX channel
that run at the same time, each tuned, filtered and gained on its own.
The driver key is `hackrfduplex`.

The HackRF One is a half-duplex radio. Two of them, with this module, are
the cheapest full-duplex SoapySDR device there is, and the first step
towards running an FDD system such as an LTE test cell on them
([Roadmap](#roadmap)).

The code is C++20, tested with Catch2. The dependencies are SoapySDR,
libhackrf and Catch2.

Documentation:

| Document | Contents |
|---|---|
| [docs/design.md](docs/design.md) | how the module is built: the pair, the stream paths, threads, timestamps, latency |
| [docs/licensing.md](docs/licensing.md) | licence and third-party works |
| [CONTRIBUTING.md](CONTRIBUTING.md) | repository layout, conventions, transmit safety |
| [TODO/ACTIVE.md](TODO/ACTIVE.md) | open work, in order |

## Status

| Component | Working | Notes |
|---|---|---|
| Discovery and opening a pair | yes | two boards pair themselves; more need `rx_serial` and `tx_serial` |
| Receive stream | yes | CS8, CS16, CF32, CF64; a stream time from the board's sample count; overflows reported |
| Transmit stream | yes | the same formats; end of burst; underflows reported through the stream status |
| Tuning, filter, gains, bias tee | yes | per direction; an overall gain is split over the stages |
| `hackrfduplex-check` | yes | receives, optionally transmits a tone, and reports rates, drops and levels |
| Verified on two boards | receive | 2, 8 and 20 Msps for 10 s without an overflow; transmit is the next item in [TODO/ACTIVE.md](TODO/ACTIVE.md) |
| Transmit timestamps, synchronised start, shared clock | no | the LTE items in the roadmap |

## Hardware

Two HackRF Ones on USB, ideally on separate host controllers: each moves
up to 40 MB/s and the HackRF is a USB 2 device. `hackrf_info` warns when
a board shares its bus.

Between the two boards, either antennas with enough separation or
attenuation for the receiver not to be overloaded, or an SMA cable with
an attenuator of at least 30 dB. The transmitter puts out up to about
+10 dBm with its amplifier on and the receiver's front end saturates far
below that.

Optional, for applications that need the two sample clocks locked: an
SMA cable from one board's CLKOUT to the other's CLKIN. The receiving
board then reports `rx_clock: external` in the hardware information.
The hardware trigger pins that start two boards together are not used
yet ([Roadmap](#roadmap)).

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

| Device argument | Meaning |
|---|---|
| `rx_serial` | suffix of the serial of the board that receives |
| `tx_serial` | suffix of the serial of the board that transmits; with only one of the two given, the other attached board takes the other role |

| Stream argument | Default | Meaning |
|---|---|---|
| `buffers` | 15 | USB transfers (256 KiB, 128 Ki samples each) the stream holds between the board and the caller |

| Setting | Default | Meaning |
|---|---|---|
| `bias_tx` | false | 3.3 V on the transmitting board's antenna port while it transmits |

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

The aim is an LTE FDD test cell on two HackRFs, as can be done today with
one BladeRF. What the pair lacks for that, in the order it will be
tackled:

1. A shared reference: CLKOUT to CLKIN between the boards, so the two
   sample clocks and both local oscillators do not drift apart.
2. A synchronised start on the hardware trigger pins, so the transmit
   stream's sample count and the receive stream's share one origin.
3. Transmit timestamps against that origin, so a sample asked for at a
   time goes out at that time.
4. Latency: libhackrf moves 256 KiB per transfer, which at LTE sample
   rates is far more than the HARQ timeline allows, and the firmware's
   own 16 KiB granularity sets the floor. Measure, then rebuild libhackrf
   with smaller transfers.
5. An srsRAN eNB on the pair at 10 MHz bandwidth.

The HackRF's 8-bit converters bound this to a bench demonstration at
modest modulation orders.

## AI

This project was developed with the use of agentic engineering,
specifically Claude.

## Licensing

Licensed under the GNU General Public License, version 3 or (at your
option) any later version; see [LICENSE](LICENSE). The project began as a
fork of SoapyHackRF, whose MIT notice is kept in [NOTICE](NOTICE).
Third-party works and their licences are listed in
[docs/licensing.md](docs/licensing.md).
