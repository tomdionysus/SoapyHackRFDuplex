#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Debian packages from the source tree it is run in, as root on
# a Debian trixie host or container: installs the build dependencies
# debian/control names, runs dpkg-buildpackage (which runs the unit
# tests), copies the .deb, .changes and .buildinfo files to OUT, and runs
# lintian over them, failing on any error or warning. GitHub Actions and
# tools/deb-amd64.sh both build the packages through it.
#
#   cd <source tree> && tools/deb-build.sh OUT

set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
[ -f debian/control ] || { echo "$0: run it from the source tree" >&2; exit 2; }
mkdir -p "$1"
out="$(cd "$1" && pwd)"
parent="$(dirname "$PWD")"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential lintian >/dev/null
apt-get build-dep -y -qq . >/dev/null

dpkg-buildpackage -us -uc -b
cp "$parent"/*.deb "$parent"/*.changes "$parent"/*.buildinfo "$out"/

# Lintian warns on stderr that it runs as root, as it must in a container;
# that line is not a finding about the packages.
lintian --fail-on error,warning "$out"/*.changes 2>&1 | tee "$out/lintian.log"
echo "built: $(cd "$out" && ls ./*.deb | tr '\n' ' ')"
