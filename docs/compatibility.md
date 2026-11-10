# Compatibility

What has been run against the pair, on the target (Raspberry Pi 5,
Debian trixie, SoapySDR 0.8.1, libhackrf 2024.02.1, both boards on
firmware 2026.01.3, `clkout=rx`), with the module from the build tree
(`SOAPY_SDR_PLUGIN_PATH`). Each row says what was checked; a quirk is
named where there is one.

| Application | Version | Checked | Result |
|---|---|---|---|
| `SoapySDRUtil --find` | 0.8.1 | discovery, without opening a board | Two devices, one per ordering of the boards: device 0 with the lower serial receiving (the default), device 1 the other way round. Either set of arguments opens that pair. |
| `SoapySDRUtil --probe` | 0.8.1 | identification, settings, sensors, ranges per channel | Everything listed as designed (COMPLETED.md, item 5): one RX and one TX channel, full duplex, four formats, CS8 native, the `buffers` stream argument, clock and time sources, `ref_locked` and `time_locked`, `lo_locked` per channel. It reports "Timestamps: NO" because no origin holds until both streams run. |
| `SoapySDRUtil --rate` | 0.8.1 | receive and transmit (zeros) separately, 8 and 16 Msps, 12 s each | 7.99 and 15.99 Msps received, no overflow; transmit 8.2 and 16.2 Msps written (the ring filling first), no underflow. |
| SoapySDR Python bindings | 0.8.1, numpy 2.2.4 | `tools/timed_loopback.py`: a timed burst 5.5 transfers after a receive timestamp, 30 bursts at each of 2, 8 and 16 Msps, TX gain 0 | Every burst written was on time to the sample (+0, 30 of 30 at 2 and 8 Msps, 29 of 29 at 16 Msps). At 16 Msps one burst was lost to a receive gap when the Python reader fell behind once; a reader in Python must drain reads from samples already queued before taking the time from one, as the script does. |
| GNU Radio, Soapy source and sink | | one full-duplex flowgraph | Not yet run: GNU Radio is not installed on the target. |
| SoapyRemote | | the pair served to another process | Not yet run: the SoapyRemote server is not installed on the target (the client module is). |
| A receive-only application | | | Not yet run: none is installed on the target. |
