#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A timed loopback through SoapySDR's Python bindings, as an application
# would write one: read a receive timestamp, transmit a coded burst a fixed
# lead after it, find the burst in what is received, and report where it
# landed against the time asked. Transmits at TX gain 0: run it only where
# transmitting is allowed.
#
#   tools/timed_loopback.py [--rate 8e6] [--freq 868.3e6] [--bursts 50]
#                           [--lead-transfers 5.5] [--args driver=hackrfduplex,clkout=rx]
#
# Exits 0 when every burst was written, and every one found was within a
# sample of its time.

import argparse
import sys

import numpy as np
import SoapySDR
from SoapySDR import (SOAPY_SDR_CF32, SOAPY_SDR_END_BURST, SOAPY_SDR_HAS_TIME,
                      SOAPY_SDR_OVERFLOW, SOAPY_SDR_RX, SOAPY_SDR_TX)

# Samples in one libhackrf transfer: the unit the transmit lead is given in.
TRANSFER = 131072
MARGIN = 2048


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--args", default="driver=hackrfduplex,clkout=rx")
    parser.add_argument("--rate", type=float, default=8e6)
    parser.add_argument("--freq", type=float, default=868.3e6)
    parser.add_argument("--rx-gain", type=float, default=32)
    parser.add_argument("--tx-gain", type=float, default=0)
    parser.add_argument("--bursts", type=int, default=50)
    parser.add_argument("--lead-transfers", type=float, default=5.5)
    options = parser.parse_args()

    device = SoapySDR.Device(options.args)
    for direction in (SOAPY_SDR_RX, SOAPY_SDR_TX):
        device.setSampleRate(direction, 0, options.rate)
        device.setFrequency(direction, 0, options.freq)
    device.setGain(SOAPY_SDR_RX, 0, options.rx_gain)
    device.setGain(SOAPY_SDR_TX, 0, options.tx_gain)
    rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)
    tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32)
    # The second activation sends the calibration burst that ties the two
    # streams to one time.
    device.activateStream(rx)
    device.activateStream(tx)
    if not device.hasHardwareTime():
        print("no common time origin: is clkout set and the reference cable in?")
        return 2

    rate = options.rate
    lead = int(options.lead_transfers * TRANSFER)
    code = np.where(np.random.default_rng(1).integers(0, 2, 1024) > 0, 0.7, -0.7)
    code = code.astype(np.complex64)
    buffer = np.zeros(device.getStreamMTU(rx), np.complex64)

    arrivals = {}
    refused = 0
    codes = {}
    lost = 0
    for _ in range(options.bursts):
        # A live read gives the time now; the burst goes a lead after it.
        # Reads from samples already queued are older than they look, so
        # they are drained first: a read is live when the hardware time is
        # within one buffer of its end.
        while True:
            result = device.readStream(rx, [buffer], len(buffer), timeoutUs=1_000_000)
            if result.ret <= 0:
                break
            tick = SoapySDR.timeNsToTicks(result.timeNs, rate)
            newest = SoapySDR.timeNsToTicks(device.getHardwareTime(), rate)
            if newest - (tick + result.ret) < len(buffer):
                break
        if result.ret <= 0:
            lost += 1
            continue
        at = SoapySDR.timeNsToTicks(result.timeNs, rate) + lead
        written = device.writeStream(tx, [code], len(code),
                                     flags=SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST,
                                     timeNs=SoapySDR.ticksToTimeNs(at, rate),
                                     timeoutUs=1_000_000)
        if written.ret != len(code):
            refused += 1
            codes[written.ret] = codes.get(written.ret, 0) + 1
            continue
        # Listen across the window it should fall in.
        start = at - MARGIN
        window = np.zeros(len(code) + 2 * MARGIN, np.complex64)
        filled = 0
        while filled < len(window):
            result = device.readStream(rx, [buffer], len(buffer), timeoutUs=1_000_000)
            if result.ret == SOAPY_SDR_OVERFLOW or result.ret <= 0:
                filled = -1
                break
            tick = SoapySDR.timeNsToTicks(result.timeNs, rate)
            low = max(tick, start)
            high = min(tick + result.ret, start + len(window))
            if high > low:
                window[low - start:high - start] = buffer[low - tick:high - tick]
                filled = high - start
        if filled < 0:
            lost += 1
            continue
        peak = int(np.argmax(np.abs(np.correlate(window, code, "valid"))))
        error = start + peak - at
        arrivals[error] = arrivals.get(error, 0) + 1

    device.deactivateStream(tx)
    device.deactivateStream(rx)
    device.closeStream(tx)
    device.closeStream(rx)

    found = sum(arrivals.values())
    print(f"timed loopback: {options.bursts} bursts at {rate / 1e6:.3f} Msps, lead "
          f"{options.lead_transfers} transfers ({lead} samples, {lead / rate * 1e3:.1f} ms)")
    print(f"  written and found {found}, refused {refused}"
          + (" (" + ", ".join(f"{SoapySDR.errToStr(c)} x{n}" for c, n in codes.items()) + ")"
             if codes else "")
          + f", lost to a gap {lost}")
    print("  arrival against the time asked, in samples: " +
          " ".join(f"{e:+d} (x{n})" for e, n in sorted(arrivals.items())))
    # A burst lost to a receive gap (the reader fell behind) says nothing
    # about its timing; one refused or found off its time does.
    ok = refused == 0 and found > 0 and all(abs(e) <= 1 for e in arrivals)
    print("  every burst on time" if ok else "  NOT every burst on time")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
