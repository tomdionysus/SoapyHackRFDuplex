#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The checks a change must pass, in order: formatting, the licence line on
# every source file, the release build and its tests, and the sanitizer
# build and its tests. Run from anywhere; it works in the repository's own
# build trees. Exits non-zero at the first failure.
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
done < <(git ls-files '*.cpp' '*.hpp' '*.in' '*.cmake' '*CMakeLists.txt' '*.sh')
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

step "passed"
