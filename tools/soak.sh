#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The soak (TODO/ACTIVE.md, item 15): full duplex with timed bursts
# throughout, at each rate in turn, through hackrfduplex-check --recover,
# with the check's resident memory and the host's power flags sampled
# every minute. Run on the target from the tree, after a release build
# (tools/ci.sh); it transmits, at TX gain 0, so only where that is allowed.
#
#   tools/soak.sh [SECONDS_PER_RATE] [RATE...]     (default 7200, 8e6 16e6)
#
# Writes build/soak/<rate>.log (the check's report, power before and
# after, its exit status, the kernel's USB and power events) and build/soak/<rate>.samples (one line a
# minute: epoch seconds, VmRSS in KiB, get_throttled). Exits 0 when every
# rate's check passed. Detach it to survive the session that started it:
#
#   setsid nohup tools/soak.sh > build/soak.out 2>&1 < /dev/null &

set -u

seconds=${1:-7200}
[ $# -gt 0 ] && shift
rates=${*:-8e6 16e6}
out=build/soak
check=./build/tools/hackrfduplex-check
mkdir -p "$out"
[ -x "$check" ] || { echo "no $check: build first (tools/ci.sh --quick)"; exit 1; }

throttled() { vcgencmd get_throttled 2>/dev/null | sed 's/throttled=//' || echo unknown; }

status=0
for rate in $rates; do
    log=$out/$rate.log
    samples=$out/$rate.samples
    # The writer's lead: 5.5 transfers of 131072 samples (docs/design.md).
    delay_us=$(python3 -c "print(round(720896 / float('$rate') * 1e6))")
    {
        echo "soak: $rate for $seconds s, timed bursts at +$delay_us us, started $(date -u +%FT%TZ)"
        echo "power before: $(throttled)"
    } >"$log"
    : >"$samples"
    started=$(date +%s)
    SOAPY_SDR_PLUGIN_PATH=$PWD/build/module "$check" --clkout rx --rate "$rate" \
        --timed-bursts "$delay_us" --seconds "$seconds" --recover --progress 60 >>"$log" 2>&1 &
    pid=$!
    tick=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ $((tick % 12)) -eq 0 ]; then
            rss=$(awk '/^VmRSS/ {print $2}' "/proc/$pid/status" 2>/dev/null)
            echo "$(date +%s) ${rss:-gone} $(throttled)" >>"$samples"
        fi
        tick=$((tick + 1))
        sleep 5
    done
    wait "$pid"
    code=$?
    {
        echo "exit: $code"
        echo "power after: $(throttled)"
        echo "finished $(date -u +%FT%TZ)"
        # USB and supply faults during the run, so a shortfall can be laid at
        # the host's door rather than the module's.
        echo "kernel USB and power events:"
        journalctl -k --since "@$started" --no-pager 2>/dev/null |
            grep -iE 'over-current|disconnect|under-voltage|xhci' || echo "  none"
    } >>"$log"
    [ "$code" -eq 0 ] || status=1
done
exit $status
