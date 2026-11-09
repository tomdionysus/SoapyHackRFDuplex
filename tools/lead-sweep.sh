#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Measures the transmit lead: the smallest delay from a receive timestamp
# to a timed transmit that runs with no late write, per sample rate and
# ring depth, through `hackrfduplex-check --timed-bursts`. Transmits
# (TX gain 0, amplifier off): run it only where transmitting is allowed.
#
# The delay steps up in quarter transfers. Each step runs STARTS short
# runs, since the threshold moves by up to a transfer between starts;
# the first step where every start is clean is confirmed with one long
# run, and the sweep steps on if that fails. Every run's output is kept,
# with the power reading (vcgencmd get_throttled, where there is one)
# before and after it.
#
#   tools/lead-sweep.sh BUILD_DIR OUT_DIR
#
#   RATES="2e6 8e6"      sample rates (default 2e6 8e6 11.52e6 15.36e6 20e6)
#   BUFFERS="default 4"  ring depths, "default" for the module's
#   STEPS="4.5 4.75"     delays to try, in transfers (default 4.5 to 8)
#   STARTS=3             short runs per step
#   SEARCH_SECONDS=30    length of a short run
#   CONFIRM_SECONDS=600  length of the confirming run

set -uo pipefail

build="${1:?usage: tools/lead-sweep.sh BUILD_DIR OUT_DIR}"
out="${2:?usage: tools/lead-sweep.sh BUILD_DIR OUT_DIR}"
rates="${RATES:-2e6 8e6 11.52e6 15.36e6 20e6}"
depths="${BUFFERS:-default 4}"
steps="${STEPS:-4.5 4.75 5 5.25 5.5 5.75 6 6.5 7 8}"
starts="${STARTS:-3}"
search_seconds="${SEARCH_SECONDS:-30}"
confirm_seconds="${CONFIRM_SECONDS:-600}"

# Samples in one libhackrf transfer of CS8.
readonly kTransfer=131072

check="$build/tools/hackrfduplex-check"
export SOAPY_SDR_PLUGIN_PATH="$build/module"
mkdir -p "$out"
summary="$out/summary.txt"

power() {
    if command -v vcgencmd >/dev/null 2>&1; then
        vcgencmd get_throttled | cut -d= -f2
    else
        echo "n/a"
    fi
}

# run RATE BUFFERS TRANSFERS SECONDS LABEL: one check run; succeeds when clean.
run() {
    local rate="$1" buffers="$2" transfers="$3" seconds="$4" label="$5"
    local delay_us
    delay_us="$(awk -v t="$transfers" -v n="$kTransfer" -v r="$rate" \
        'BEGIN { printf "%.1f", t * n / r * 1e6 }')"
    local file
    file="$(printf '%s/r%s_b%s_d%s_s%s_%s.txt' "$out" "$rate" "$buffers" "$transfers" \
        "$seconds" "$label")"
    local args=(--clkout rx --tx-gain 0 --rate "$rate" --timed-bursts "$delay_us"
        --seconds "$seconds")
    if [[ "$buffers" != "default" ]]; then
        args+=(--buffers "$buffers")
    fi
    local before after status
    before="$(power)"
    "$check" "${args[@]}" >"$file" 2>&1
    status=$?
    after="$(power)"
    echo "power: $before before, $after after" >>"$file"
    local late overflows verdict
    late="$(sed -n 's/^timed: .* \([0-9]*\) late writes.*/\1/p' "$file")"
    overflows="$(sed -n 's/.*(\([0-9]*\) overflows)$/\1/p' "$file")"
    verdict="$([[ $status -eq 0 ]] && echo clean || echo "NOT clean")"
    printf '%-9s %-7s %5s transfers %4ss %-8s %-9s late %-6s overflows %-4s power %s -> %s\n' \
        "$rate" "$buffers" "$transfers" "$seconds" "$label" "$verdict" "${late:-?}" \
        "${overflows:-?}" "$before" "$after" | tee -a "$summary"
    return "$status"
}

echo "lead sweep $(date -u +%FT%TZ): rates $rates; buffers $depths; steps $steps;" \
    "$starts starts of ${search_seconds}s; confirm ${confirm_seconds}s" | tee -a "$summary"
results=()
for rate in $rates; do
    for buffers in $depths; do
        found=""
        for transfers in $steps; do
            clean=1
            for ((i = 1; i <= starts; ++i)); do
                run "$rate" "$buffers" "$transfers" "$search_seconds" "start$i" || clean=0
            done
            if ((clean)) && run "$rate" "$buffers" "$transfers" "$confirm_seconds" confirm; then
                found="$transfers"
                break
            fi
        done
        if [[ -n "$found" ]]; then
            results+=("$(awk -v r="$rate" -v b="$buffers" -v t="$found" -v n="$kTransfer" \
                'BEGIN { printf "%7.2f Msps  buffers %-7s  %5s transfers = %6.1f ms", r / 1e6, b, t, t * n / r * 1e3 }')")
        else
            results+=("$(awk -v r="$rate" -v b="$buffers" \
                'BEGIN { printf "%7.2f Msps  buffers %-7s  none clean", r / 1e6, b }')")
        fi
    done
done

echo "results:" | tee -a "$summary"
for line in "${results[@]}"; do
    echo "  $line" | tee -a "$summary"
done
