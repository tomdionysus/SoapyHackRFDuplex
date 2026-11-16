#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the libFuzzer targets in fuzz/ into OUT, each linked with the
# module's core (every source in module/src but the libhackrf binding,
# the SoapySDR translation and registration, and the receive corrections),
# which needs neither libhackrf nor SoapySDR. Takes the compiler and flags
# from the environment as ClusterFuzzLite's builder sets them (CXX,
# CXXFLAGS, LIB_FUZZING_ENGINE); without them, Clang with libFuzzer,
# AddressSanitizer and UndefinedBehaviorSanitizer.
#
#   tools/fuzz-build.sh OUT

set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$1"
out="$(cd "$1" && pwd)"
objects="$out/objects"
mkdir -p "$objects"

cxx="${CXX:-clang++}"
cxxflags="${CXXFLAGS:--g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=all}"
engine="${LIB_FUZZING_ENGINE:--fsanitize=fuzzer}"
# shellcheck disable=SC2206  # the flags are words, as make would split them
flags=($cxxflags -std=c++20 -pthread -I"$root/module/src")

core=()
for source in "$root"/module/src/*.cpp; do
    case "$(basename "$source")" in
        hackrf_board.cpp | soapy_device.cpp | registration.cpp | rx_correction.cpp) continue ;;
    esac
    object="$objects/$(basename "$source" .cpp).o"
    "$cxx" "${flags[@]}" -c "$source" -o "$object"
    core+=("$object")
done

for target in "$root"/fuzz/fuzz_*.cpp; do
    name="$(basename "$target" .cpp)"
    # shellcheck disable=SC2086  # the engine is flags, as above
    "$cxx" "${flags[@]}" "$target" "${core[@]}" $engine -o "$out/$name"
    echo "built $out/$name"
done
