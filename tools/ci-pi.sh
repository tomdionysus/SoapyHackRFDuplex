#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and tests on the target, a Debian host with the two boards
# attached, over ssh: the working tree (uncommitted changes included,
# build trees left out) is copied there, then a command runs in the
# copy. The target's address and directory come from the environment and
# never from the repository.
#
#   HACKRFDUPLEX_TARGET=user@host   the target (required)
#   HACKRFDUPLEX_TARGET_DIR=path    where the tree lives there
#                                   (default src/SoapyHackRFDuplex)
#
#   tools/ci-pi.sh                  sync, then tools/ci.sh there
#   tools/ci-pi.sh --sync           sync only
#   tools/ci-pi.sh -- CMD...        sync, then CMD in the tree there

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
target="${HACKRFDUPLEX_TARGET:?set HACKRFDUPLEX_TARGET to user@host}"
dir="${HACKRFDUPLEX_TARGET_DIR:-src/SoapyHackRFDuplex}"

ssh -o BatchMode=yes "$target" "mkdir -p '$dir'"
rsync -a --delete --no-owner --no-group \
    --exclude '/build/' --exclude '/build-*/' --exclude '.cache/' --exclude '.DS_Store' \
    --exclude '/obj-*/' --exclude '/debian/.debhelper/' --exclude '/debian/tmp/' \
    "$root/" "$target:$dir/"

if [[ "${1:-}" == "--sync" ]]; then
    exit 0
fi
if [[ "${1:-}" == "--" ]]; then
    shift
    command="$(printf '%q ' "$@")"
else
    command="tools/ci.sh"
fi
exec ssh -o BatchMode=yes "$target" "cd '$dir' && $command"
