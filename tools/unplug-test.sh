#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Acceptance for a board lost mid-stream: run the check tool, take one
# board off the bus by deauthorising its USB device in sysfs, and record
# how long the tool takes to see it, then put the board back. Receive
# only unless the check options given transmit. Needs root.
#
# Usage: tools/unplug-test.sh BUILD_DIR OUT_DIR rx|tx [check tool options]
# Env: RATE (2e6), RUN_SECONDS before the unplug (3).
set -euo pipefail

usage="usage: tools/unplug-test.sh BUILD_DIR OUT_DIR rx|tx [check options]"
build=${1:?$usage}
out=${2:?$usage}
which=${3:?$usage}
shift 3
[[ "$which" == rx || "$which" == tx ]] || { echo "$usage" >&2; exit 2; }
rate=${RATE:-2e6}
run_seconds=${RUN_SECONDS:-3}
mkdir -p "$out"

power() { vcgencmd get_throttled 2>/dev/null || echo "throttled=unknown"; }
now() { date +%s.%N; }

echo "power before: $(power)" | tee "$out/summary.txt"
start=$(now)
# Line-buffered, so the report the serials come from is there to read.
SOAPY_SDR_PLUGIN_PATH="$build/module" stdbuf -oL -eL "$build/tools/hackrfduplex-check" \
    --rate "$rate" --seconds 30 "$@" >"$out/check.log" 2>&1 &
pid=$!
sleep "$run_seconds"
serial=$(sed -n "s/^  ${which}_serial: //p" "$out/check.log")
if [[ -z "$serial" ]]; then
    kill "$pid" 2>/dev/null || true
    echo "no ${which}_serial in check.log" | tee -a "$out/summary.txt"
    exit 1
fi
device=""
for d in /sys/bus/usb/devices/*; do
    if [[ -f "$d/serial" && "$(cat "$d/serial")" == "$serial" ]]; then
        device=$d
    fi
done
if [[ -z "$device" ]]; then
    kill "$pid" 2>/dev/null || true
    echo "no USB device with serial $serial" | tee -a "$out/summary.txt"
    exit 1
fi

unplugged=$(now)
echo 0 >"$device/authorized"
status=0
wait "$pid" || status=$?
ended=$(now)
echo 1 >"$device/authorized"

python3 -c "
start, unplugged, ended = $start, $unplugged, $ended
print(f'$which board ($device) deauthorised {unplugged - start:.1f} s after the start')
print(f'check tool ended {ended - unplugged:.2f} s after the unplug, exit status $status')
" | tee -a "$out/summary.txt"
sleep 2
if [[ "$(cat "$device/authorized")" == 1 && -f "$device/serial" ]]; then
    echo "$which board authorised again" | tee -a "$out/summary.txt"
else
    echo "$which board NOT back on the bus" | tee -a "$out/summary.txt"
fi
echo "power after: $(power)" | tee -a "$out/summary.txt"
