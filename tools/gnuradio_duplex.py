#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One full-duplex GNU Radio flowgraph on the pair, as a GNU Radio user
# would build it: a Soapy sink transmitting a tone and a Soapy source
# receiving it, both on driver=hackrfduplex, in one top block. GNU Radio
# opens the source and the sink each with its own SoapySDR::Device::make;
# SoapySDR hands both the one device. Transmits at TX gain 0: run it only
# where transmitting is allowed.
#
#   tools/gnuradio_duplex.py [--rate 2e6] [--freq 868.3e6] [--seconds 10]
#                            [--rx-stream-args buffers=40]
#
# Exits 0 when the strongest signal away from the centre, at the end of
# the run, is the tone, where it was sent and well above the noise.

import argparse
import sys
import time

import numpy as np
from gnuradio import analog, blocks, gr, soapy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--args", default="driver=hackrfduplex,clkout=rx")
    parser.add_argument("--rate", type=float, default=2e6)
    parser.add_argument("--freq", type=float, default=868.3e6)
    parser.add_argument("--offset", type=float, default=250e3)
    parser.add_argument("--rx-gain", type=float, default=20)
    parser.add_argument("--tx-gain", type=float, default=0)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--rx-stream-args", default="",
                        help="e.g. buffers=40, the receive ring's depth in transfers")
    options = parser.parse_args()

    capture = 1 << 16
    tb = gr.top_block()
    tone = analog.sig_source_c(options.rate, analog.GR_COS_WAVE, options.offset, 0.5)
    sink = soapy.sink(options.args, "fc32", 1, "", "", [""], [""])
    source = soapy.source(options.args, "fc32", 1, "", options.rx_stream_args, [""], [""])
    for block, gain in ((sink, options.tx_gain), (source, options.rx_gain)):
        block.set_sample_rate(0, options.rate)
        block.set_frequency(0, options.freq)
        block.set_gain(0, gain)
    # The last `capture` samples before the end of the run.
    skip = blocks.skiphead(gr.sizeof_gr_complex,
                            int(options.rate * options.seconds) - capture)
    head = blocks.head(gr.sizeof_gr_complex, capture)
    received = blocks.vector_sink_c()
    tb.connect(tone, sink)
    tb.connect(source, skip, head, received)

    tb.start()
    deadline = time.monotonic() + options.seconds + 5
    while len(received.data()) < capture and time.monotonic() < deadline:
        time.sleep(0.1)
    tb.stop()
    tb.wait()

    x = np.array(received.data(), dtype=np.complex64)
    if len(x) < capture:
        print(f"received {len(x)} of {capture} samples by the end of the run")
        return 1
    spectrum = np.abs(np.fft.fftshift(np.fft.fft(x * np.hanning(len(x))))) ** 2
    freqs = np.fft.fftshift(np.fft.fftfreq(len(x), 1 / options.rate))
    # The receiver's DC spike sits at the centre (a module built without
    # the receive corrections leaves it), so the search leaves it out.
    searched = np.where(np.abs(freqs) > 20e3, spectrum, 0)
    peak = int(np.argmax(searched))
    noise = float(np.median(spectrum))
    snr = 10 * np.log10(spectrum[peak] / noise)
    error = freqs[peak] - options.offset
    bin_width = options.rate / len(x)
    print(f"{options.seconds:g} s at {options.rate / 1e6:g} Msps: tone at "
          f"{freqs[peak] / 1e3:+.1f} kHz (sent {options.offset / 1e3:+.1f} kHz), "
          f"{snr:.1f} dB above the median bin")
    return 0 if abs(error) <= 2 * bin_width and snr > 20 else 1


if __name__ == "__main__":
    sys.exit(main())
