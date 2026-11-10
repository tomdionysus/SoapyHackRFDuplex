# Examples

## hackrfduplex-ft8

An FT8 decoder that uses the pair as an ordinary SoapySDR application
would, in `examples/ft8`. FT8 is a weak-signal digital mode: a 15 s slot
carries a short message as eight-tone frequency-shift keying in about
50 Hz, and the decoder finds every message in a slot by correlation and
error-correcting decoding. A message decodes only if the samples are
right, so it is a sharp end-to-end test of the receive path, and with
`--loopback` of timed transmit too.

The FT8 code comes from corvus-radioberry's `ft8rx`, a C++ port of
kgoba/ft8_lib (docs/licensing.md); what is new here is the SoapySDR
client and the resampling between the pair's 1.92 Msps and the 48 ksps
the FT8 chain starts at (`src/resampler.*`). It needs liquid-dsp and
builds with the receive corrections' preset, or alone:

```
cmake -S . -B build -DHACKRFDUPLEX_BUILD_EXAMPLES=ON
cmake --build build -j
```

It is not packaged.

Two ways to run it:

```
hackrfduplex-ft8 --dial 144174000            # receive only, until interrupted
hackrfduplex-ft8 --loopback --dial 868298500 # transmit a frame each slot and decode it
```

Receive only, it decodes each UTC 15 s slot from the dial frequency and
prints one line per message as WSJT-X does: the slot's time, SNR in
2500 Hz, the time offset in seconds, the audio frequency, and the text.
It needs an antenna and a band with FT8 on it (144.174 MHz is the 2 m
FT8 frequency) and the host's clock on UTC within a second. The receive
centre sits 1500 Hz above the dial, so FT8's 3 kHz of audio lies either
side of it; the receiver's DC spike sits at the centre, and a module
built with the receive corrections removes it.

`--loopback` transmits, at TX gain 0 unless `--tx-gain` says otherwise:
only where transmitting is allowed. Each slot it schedules `--message`
(standard FT8 text, "CQ K1ABC FN42" by default) as a timed burst a second
ahead, 0.5 s into a slot as FT8 frames start, receives the slot and
decodes it, and exits 0 when every slot gave the message back.

On the target, 2026-11-10, `--loopback --dial 868298500 --slots 3`, the
two boards stacked with stub antennas: the message decoded in 3 slots of
3, at +17 to +18 dB SNR, at 1000 Hz exactly where it was sent, and at a
time offset of 0.0 s, which is the timed transmit landing on time.
Receive only at 144.174 MHz on the shielded bench decodes nothing, as it
should, three whole slots of 48 000 samples each.
