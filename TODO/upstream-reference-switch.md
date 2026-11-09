# Draft issue for greatscottgadgets/hackrf

A draft for Tom to file if he wants; nothing has been posted. Written
against the 2026.01.3 release.

---

**Title:** A stream start that changes the clock source leaves the RF
synthesisers on the old reference (and a retune to the same frequency
is skipped)

**Firmware:** 2026.01.3 release, HackRF One (seen on a board reporting
id `HACKRF1_OG` and on an older pre-r6 board; the code path is common
to all non-Praline boards).

**What happens**

When a stream starts and the firmware finds a different clock source
from the one it was using (CLKIN has appeared or gone since the last
start), the board streams at the full sample rate but its RF is dead:
a receiving board hears nothing at the tuned frequency, a transmitting
board puts nothing out. The sample stream itself looks healthy, so
nothing on the host notices. Retuning by hand does not help if it is to
the same frequency. Retuning to another frequency and back, or a reset,
cures it.

**Why, as far as the source shows**

`transceiver_startup()` (`firmware/hackrf_usb/usb_api_transceiver.c`)
calls `radio_switch_mode()` first and `activate_best_clock_source()`
after it:

```c
void transceiver_startup(const transceiver_mode_t mode)
{
	radio_switch_mode(&radio, RADIO_CHANNEL0, mode);
	...
	activate_best_clock_source();
	trigger_enable(radio_get_trigger_enable(&radio, RADIO_CHANNEL0));
}
```

`radio_switch_mode()` (`firmware/common/radio.c`) applies the sample
rate and then the frequency, so the synthesisers are tuned on whatever
reference the Si5351C was running from before. `activate_best_clock_source()`
then moves the Si5351C to CLKIN or to its crystal. The sample clocks
follow the new reference at once; the RFFC5072 and MAX2837 synthesisers
are not retuned and do not relock.

A retune from the host to the same frequency does not reach the
synthesisers either, because `radio_set_frequency()` skips a tune to
the frequency it last applied:

```c
static uint64_t applied_freq = 0xffffffffffffffff;
...
	// auto-tune
	if (frequency.hz == applied_freq) {
		return RADIO_OK;
	}
	applied_freq = frequency.hz;
```

`applied_freq` is not reset when the clock source changes.

**To reproduce**

Two HackRF Ones, A and B, with an SMA cable from A's CLKOUT to B's
CLKIN (and nothing fitted on B's CLKIN header), and a signal B should
hear at a known frequency F (A transmitting, or any steady carrier).

1. With A's CLKOUT off (`hackrf_clock -d <A> -o 0`), receive on B at F
   with `hackrf_transfer -d <B> -r b1.cs8 -f <F> -s 8000000 -n 8000000`.
   The carrier is there. B's synthesisers are now tuned on its crystal.
2. Turn A's CLKOUT on: `hackrf_clock -d <A> -o 1`. `hackrf_clock -d <B> -i`
   now reports a clock signal on CLKIN.
3. Receive on B again at the same F, as in step 1. The stream runs at
   the full rate, but the carrier is gone.
4. Receive at F + 1 MHz and then at F again (or reset B): the carrier is
   back.

The same happens in the other direction (CLKOUT turned off between two
starts), and transmitting instead of receiving: the transmitter puts
out nothing until it is retuned to a different frequency.

**Suggested fix**

Either choose the clock source before tuning, by calling
`activate_best_clock_source()` at the start of `transceiver_startup()`
rather than at its end, or, if a source change is detected there, reset
`applied_freq` and apply the frequency again after switching. Choosing
the source first seems the simpler and also keeps the sample-rate
setup on the final reference.

**Workaround used meanwhile**

Before starting a stream, the host compares the CLKIN status
(`hackrf_get_clkin_status`) with the source the Si5351C is running from
now (CLK0's PLL source bit on boards clocked the older way, the PLL A
multiplier on `HACKRF1_R9`, read with `hackrf_si5351c_read` while the
board is idle, since the registers read as zero while it streams). When
the two differ, it starts the stream, tunes 1 MHz away and back, stops
and starts again.

A separate observation from the same work: a retune to another
frequency and back while nothing had switched left one board's receiver
offset by 1.7 kHz until it was reset. That is not the bug above; it is
mentioned in case it is related.
