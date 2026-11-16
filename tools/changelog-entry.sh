#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints one version's entry from a changelog in this repository's form
# ("## x.y.z - date", then the entry, up to the next "## "), without its
# heading: the notes of that version's GitHub Release. Fails when the
# version has no entry.
#
#   tools/changelog-entry.sh VERSION [CHANGELOG.md]

set -euo pipefail

[ $# -ge 1 ] || { echo "usage: $0 VERSION [CHANGELOG.md]" >&2; exit 2; }
version="$1"
file="${2:-CHANGELOG.md}"

found=0
printing=0
while IFS= read -r line; do
    if [[ "$line" == "## "* ]]; then
        if [[ $printing -eq 1 ]]; then
            break
        fi
        if [[ "$line" == "## $version "* || "$line" == "## $version" ]]; then
            found=1
            printing=1
            continue
        fi
    fi
    if [[ $printing -eq 1 ]]; then
        printf '%s\n' "$line"
    fi
done <"$file"

if [[ $found -eq 0 ]]; then
    echo "$0: no entry for $version in $file" >&2
    exit 1
fi
