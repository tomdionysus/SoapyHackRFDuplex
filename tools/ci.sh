#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The checks a change must pass, in order: formatting, the licence line on
# every source file, the release build and its tests, the address and
# undefined-behaviour sanitizer build and its tests, the build with the
# receive corrections (HACKRFDUPLEX_DSP) and its tests, a Clang build for
# the thread-safety analysis where Clang is not the default compiler,
# clang-tidy where the host has it, the coverage summary where lcov is,
# and the thread sanitizer build and its tests where the host can run it. Run
# from anywhere; it works in the repository's own build trees. Exits
# non-zero at the first failure.
#
#   tools/ci.sh            everything
#   tools/ci.sh --quick    formatting, licence lines, release build and tests

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

quick=0
if [[ "${1:-}" == "--quick" ]]; then
    quick=1
fi

jobs="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

step() {
    printf '\n== %s\n' "$*"
}

step "format"
mapfile -t sources < <(git ls-files '*.cpp' '*.hpp')
if [[ ${#sources[@]} -eq 0 ]]; then
    echo "git lists no sources; is this a git checkout git will read?"
    exit 1
fi
clang-format --dry-run --Werror "${sources[@]}"
echo "${#sources[@]} files formatted"

step "licence lines"
missing=0
checked=0
while IFS= read -r file; do
    checked=$((checked + 1))
    first=""
    second=""
    {
        IFS= read -r first || true
        IFS= read -r second || true
    } <"$file"
    # A script's first line is its interpreter.
    case "$first$second" in
        *"SPDX-License-Identifier: GPL-3.0-or-later"*) ;;
        *)
            echo "no SPDX licence line: $file"
            missing=1
            ;;
    esac
done < <(git ls-files '*.cpp' '*.hpp' '*.in' '*.cmake' '*CMakeLists.txt' '*.sh' '*.py' '*.1')
if [[ $missing -ne 0 || $checked -eq 0 ]]; then
    exit 1
fi
echo "all $checked source files carry their licence line"

step "release build"
cmake --preset default >/dev/null
cmake --build build -j "$jobs"
step "release tests"
./build/module/hackrfduplex_tests

if [[ $quick -eq 1 ]]; then
    step "passed (quick)"
    exit 0
fi

step "sanitizer build"
cmake --preset asan >/dev/null
cmake --build build-asan -j "$jobs"
step "sanitizer tests"
# LeakSanitizer is not available on macOS.
leaks=1
if [[ "$(uname -s)" == "Darwin" ]]; then
    leaks=0
fi
ASAN_OPTIONS="detect_leaks=$leaks:abort_on_error=1" \
    UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
    ./build-asan/module/hackrfduplex_tests

# The receive corrections are built only on request (HACKRFDUPLEX_DSP,
# with liquid-dsp); both builds must pass.
step "DSP build (receive corrections, liquid-dsp)"
cmake --preset dsp >/dev/null
cmake --build build-dsp -j "$jobs"
step "DSP tests"
./build-dsp/module/hackrfduplex_tests
./build-dsp/examples/ft8/ft8_tests

# Clang's thread-safety analysis proves the locking annotations
# (module/src/mutex.hpp); GCC ignores them. Where the default compiler is
# not Clang, the tree is built once more with it.
if "${CXX:-c++}" --version 2>/dev/null | grep -q clang; then
    step "thread-safety analysis: carried by the release build (Clang)"
else
    clang_cxx=""
    for candidate in clang++-19 clang++; do
        if command -v "$candidate" >/dev/null; then
            clang_cxx="$candidate"
            break
        fi
    done
    if [[ -z "$clang_cxx" ]]; then
        echo "no clang++ to run the thread-safety analysis with"
        exit 1
    fi
    step "thread-safety analysis ($clang_cxx build)"
    # With the receive corrections, so every source file is analysed.
    cmake -S . -B build-clang -DCMAKE_CXX_COMPILER="$clang_cxx" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHACKRFDUPLEX_DSP=ON >/dev/null
    cmake --build build-clang -j "$jobs"
fi

# clang-tidy with the checked-in .clang-tidy, over the module, the tests and
# the check tool. The target has it; a host without it says so.
tidy=""
for candidate in run-clang-tidy-19 run-clang-tidy; do
    if command -v "$candidate" >/dev/null; then
        tidy="$candidate"
        break
    fi
done
if [[ -z "$tidy" ]]; then
    step "clang-tidy: not on this host, skipped"
else
    database=build
    if [[ -f build-clang/compile_commands.json ]]; then
        database=build-clang
    fi
    step "clang-tidy ($tidy, $database)"
    "$tidy" -p "$database" -quiet -j "$jobs" 'module/src/.*|module/tests/.*|tools/.*' \
        >"$database/clang-tidy.log" 2>&1 || {
        grep -E "(warning|error):" "$database/clang-tidy.log" || tail -n 20 "$database/clang-tidy.log"
        exit 1
    }
    echo "clean"
fi

# Line coverage of the unit-tested sources (cmake/Instrumentation.cmake
# lists what is left out and why), where lcov is installed.
if command -v lcov >/dev/null && command -v genhtml >/dev/null; then
    step "coverage"
    cmake --preset coverage >/dev/null
    cmake --build build-coverage -j "$jobs"
    cmake --build build-coverage --target coverage >build-coverage/coverage.log 2>&1 || {
        tail -n 20 build-coverage/coverage.log
        exit 1
    }
    grep -A3 "Summary coverage" build-coverage/coverage.log
else
    step "coverage: lcov not on this host, skipped"
fi

# ThreadSanitizer cannot run on every host: on arm64 it needs a 39, 42 or
# 48-bit address space, and a Raspberry Pi 5 kernel with 16K pages gives 47.
# A host that cannot run a sanitized program skips the step, saying why.
mkdir -p build-tsan-probe
printf 'int main() { return 0; }\n' >build-tsan-probe/probe.cpp
if ! "${CXX:-c++}" -fsanitize=thread build-tsan-probe/probe.cpp -o build-tsan-probe/probe \
    >build-tsan-probe/probe.log 2>&1 ||
    ! ./build-tsan-probe/probe >>build-tsan-probe/probe.log 2>&1; then
    step "thread sanitizer: not available on this host, skipped"
    while IFS= read -r line; do echo "$line"; done <build-tsan-probe/probe.log
    step "passed (without the thread sanitizer)"
    exit 0
fi

step "thread sanitizer build"
cmake --preset tsan >/dev/null
cmake --build build-tsan -j "$jobs"
step "thread sanitizer tests"
TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" ./build-tsan/module/hackrfduplex_tests

step "passed"
