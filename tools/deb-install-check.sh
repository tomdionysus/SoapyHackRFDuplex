#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs the module and tools packages from DIR, with SoapySDR's own
# tools, as root on a clean Debian trixie host or container, and checks
# that SoapySDR loads the module and the check tool runs. No board is
# needed. GitHub Actions and tools/deb-amd64.sh both check the packages
# through it.
#
#   tools/deb-install-check.sh DIR

set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 DIR" >&2; exit 2; }
dir="$(cd "$1" && pwd)"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq soapysdr-tools \
    "$dir"/soapysdr0.8-module-hackrfduplex_*.deb \
    "$dir"/hackrfduplex-tools_*.deb >/dev/null

dpkg -l "*hackrfduplex*" | grep "^ii"
info="$(SoapySDRUtil --info)"
grep -i "hackrfduplex" <<<"$info"
# A module that fails to load is still listed as found; its factory is not.
grep -q "^Available factories\.\.\..*\bhackrfduplex\b" <<<"$info"
hackrfduplex-check --help >/dev/null
echo "hackrfduplex-check runs"
