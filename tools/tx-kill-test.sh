#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Acceptance for the transmit underrun limit: transmit a tone through the
# check tool, kill the tool with SIGKILL while it transmits, then read the
# transmitting board's M0 state and listen on the receiving board alone
# for the tone. With the limit the board has stopped by itself: M0 idle
# with a TX timeout, and no tone. Transmits, at TX gain 0.
#
# Usage: tools/tx-kill-test.sh BUILD_DIR OUT_DIR [check tool options]
# Env: RATE (2e6), TONE (200e3), FREQ (868.3e6), RUN_SECONDS before the
# kill (5). The limit is the module's default.
set -euo pipefail

build=${1:?usage: tools/tx-kill-test.sh BUILD_DIR OUT_DIR [check options]}
out=${2:?usage: tools/tx-kill-test.sh BUILD_DIR OUT_DIR [check options]}
shift 2
rate=${RATE:-2e6}
tone=${TONE:-200e3}
freq=${FREQ:-868.3e6}
run_seconds=${RUN_SECONDS:-5}
mkdir -p "$out"

power() { vcgencmd get_throttled 2>/dev/null || echo "throttled=unknown"; }

echo "power before: $(power)" | tee "$out/summary.txt"
# Line-buffered, or the kill loses the report the serials come from.
SOAPY_SDR_PLUGIN_PATH="$build/module" stdbuf -oL -eL "$build/tools/hackrfduplex-check" \
    --rate "$rate" --rx-freq "$freq" --tx-freq "$freq" --tx-tone "$tone" \
    --seconds 600 "$@" >"$out/check.log" 2>&1 &
pid=$!
sleep "$run_seconds"
if ! kill -0 "$pid" 2>/dev/null; then
    echo "the check tool ended before the kill; see check.log" | tee -a "$out/summary.txt"
    exit 1
fi
kill -KILL "$pid"
wait "$pid" 2>/dev/null || true
killed_at=$(date +%s.%N)
echo "killed after ${run_seconds} s" | tee -a "$out/summary.txt"
sleep 0.5

rx_serial=$(sed -n 's/^  rx_serial: //p' "$out/check.log")
tx_serial=$(sed -n 's/^  tx_serial: //p' "$out/check.log")
if [[ -z "$rx_serial" || -z "$tx_serial" ]]; then
    echo "no serials in check.log; the tool did not get as far as the hardware report" |
        tee -a "$out/summary.txt"
    exit 1
fi

hackrf_debug -d "$tx_serial" -S >"$out/tx-state.txt" 2>&1 || true
echo "tx M0 state after the kill:" | tee -a "$out/summary.txt"
sed 's/^/  /' "$out/tx-state.txt" | tee -a "$out/summary.txt"

samples=$(python3 -c "print(int(float('$rate')))")
hackrf_transfer -d "$rx_serial" -r "$out/after.cs8" -f "$(python3 -c "print(int(float('$freq')))")" \
    -s "$samples" -n "$samples" -l 16 -g 20 -a 0 >"$out/listen.log" 2>&1
listened_at=$(date +%s.%N)

python3 - "$out/after.cs8" "$rate" "$tone" <<'EOF' | tee -a "$out/summary.txt"
import sys
import numpy as np
path, rate, tone = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
raw = np.fromfile(path, dtype=np.int8).astype(np.float32)
iq = (raw[0::2] + 1j * raw[1::2])[len(raw) // 4:]
n = 1 << 16
spectra = [np.abs(np.fft.fftshift(np.fft.fft(iq[i:i + n] * np.hanning(n)))) ** 2
           for i in range(0, len(iq) - n + 1, n)]
mean = np.mean(spectra, axis=0)
bin_hz = rate / n
at = int(round(tone / bin_hz)) + n // 2
peak = np.max(mean[at - 3:at + 4])
noise = np.median(mean)
print(f"listening after the kill: tone at {tone / 1e3:+.1f} kHz is "
      f"{10 * np.log10(peak / noise):.1f} dB over the median bin")
EOF
python3 -c "print(f'listened {float(\"$listened_at\") - float(\"$killed_at\"):.1f} s after the kill (1 s of samples)')" |
    tee -a "$out/summary.txt"
echo "power after: $(power)" | tee -a "$out/summary.txt"
