# Quickstart

From two HackRF Ones to a full-duplex SoapySDR device, checked.

## 1. Connect the boards

- Two HackRF Ones on firmware with USB API 1.06 or later
  (`hackrf_info` shows it), each on its own USB host controller where the
  host has two.
- An SMA cable from one board's CLKOUT to the other's CLKIN, so both run
  from one reference. A board with a TCXO module on its CLKIN header
  cannot also take the cable: put the cable into the other board.
- Antennas on both, close together, or a cable between them through at
  least 30 dB of attenuation. The boards must hear each other, and must
  not overload each other.
- Enough power. A Raspberry Pi 5 needs its 5 A supply for two HackRFs.

[usage.md](usage.md#setting-up) explains each of these.

## 2. Install

On Debian trixie, build and install the packages:

```
sudo apt install build-essential debhelper cmake pkgconf libhackrf-dev libsoapysdr-dev catch2
git clone https://github.com/tomdionysus/SoapyHackRFDuplex.git
cd SoapyHackRFDuplex
dpkg-buildpackage -us -uc -b
sudo apt install ../soapysdr0.8-module-hackrfduplex_*.deb ../hackrfduplex-tools_*.deb
```

Other systems: [building.md](building.md).

## 3. Find the pair

```
SoapySDRUtil --find="driver=hackrfduplex"
```

It lists the pair twice, once for each choice of which board receives.
Device 0, with the lower serial receiving, is the default.

## 4. Check it

Receive only:

```
hackrfduplex-check --clkout rx --seconds 5
```

The report should show about 100% of the nominal rate, 0 overflows, and
`clock: external` on the board the cable feeds. If the cable runs from
the transmitting board, use `--clkout tx`.

Then, where transmitting is allowed, both boards together, at the lowest
transmit gain:

```
hackrfduplex-check --clkout rx --tx-tone 100e3 --tx-gain 0
hackrfduplex-check --clkout rx --timed-bursts 90112 --seconds 60
```

The first finds the tone where it was sent. The second sends timed
bursts and should end with "every burst on time and in place".

To certify a pair in one go:

```
hackrfduplex-check --clkout rx --acceptance --transmit
```

## 5. Use it

Open `driver=hackrfduplex,clkout=rx` from any SoapySDR application.
[usage.md](usage.md) covers the shared time, transmitting at a time and
the stream statuses; [reference.md](reference.md) lists every argument
and setting; [compatibility.md](compatibility.md) lists the applications
it has been run with.
