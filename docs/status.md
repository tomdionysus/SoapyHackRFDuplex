# Status

What works, as checked on a Raspberry Pi 5 with two HackRF Ones
(firmware 2026.01.3, libhackrf 2024.02.1, one TCXO as the shared
reference). The measurements are in [design.md](design.md).

| Feature | Status | Notes |
|---|---|---|
| Discovery and opening a pair | works | two boards pair themselves; more need `rx_serial` and `tx_serial` |
| Receive stream | works | CS8, CS16, CF32, CF64; timestamps from the board's sample count; direct buffer access in CS8 |
| Transmit stream | works | the same formats; bursts; underflows reported |
| Tuning, filters, gains, bias tee | works | per direction, neither disturbing the other |
| Shared reference clock | works | `clkout`; a board whose reference changes at a stream start is retuned and restarted |
| Shared time and timed transmit | works | from a calibration burst when both streams start; each burst received at the sample asked |
| Timed and finite receive | works | |
| Fault reporting | works | lost samples, a stopped or unplugged board, a lost origin, each as a stream status within about 100 ms |
| Transmitter safety | works | the transmitting board stops by itself if its host stops feeding it |
| Sensors | works | `lo_locked`, `ref_locked`, `time_locked` |
| Receive DC and I/Q correction | works | optional build, with liquid-dsp |
| Full duplex at 16 Msps | works | a two-hour soak with timed bursts throughout, as at 8 Msps |
| Full duplex at 20 Msps | host limit | the Pi 5's USB cannot carry 20 Msps cleanly in either direction, with or without this module |
| Trigger time origin | untested on hardware | both boards started on one edge through a jumper; tested against simulated boards |
| Reference switching on boards that identify as `HACKRF1_R9` | untested on hardware | genuine r9 and later boards select their reference differently; that path is built but has not met such a board |

Applications run with the pair: [compatibility.md](compatibility.md).
