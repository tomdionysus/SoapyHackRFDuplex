#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Debian packages for amd64 in a clean debian:trixie container,
# from a tag (default: the latest), runs lintian over them, then installs
# them into a second clean container and checks that SoapySDR loads the
# module, through tools/deb-build.sh and tools/deb-install-check.sh as CI
# does. Needs Docker; the containers see only the output directory.
#
#   tools/deb-amd64.sh [TAG]
#
# Writes build-deb-amd64/<tag>/: the source the packages were built from
# (source.tar, from git archive), the two scripts, the .deb, .changes and
# .buildinfo files, build.log, lintian.log and install.log. Exits non-zero
# on any failure.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

ref="${1:-$(git describe --tags --abbrev=0)}"
out="build-deb-amd64/$ref"
image=debian:trixie
mkdir -p "$out"
find "$out" -maxdepth 1 -type f -delete

# The tagged tree, without .git: the version comes from the project's
# own number, as it does for any build from a release archive. The build
# and install scripts are this checkout's, so an older tag builds the same
# way.
git archive --format=tar --prefix=src/ "$ref" >"$out/source.tar"
cp tools/deb-build.sh tools/deb-install-check.sh "$out/"

echo "== build $ref for amd64 in $image"
docker run --rm --platform linux/amd64 -v "$root/$out:/out" "$image" bash -euc '
    mkdir /work
    tar -xf /out/source.tar -C /work
    cd /work/src
    bash /out/deb-build.sh /out
' >"$out/build.log" 2>&1 || {
    echo "build failed; the end of $out/build.log:"
    tail -n 30 "$out/build.log"
    exit 1
}
echo "built: $(cd "$out" && ls ./*.deb | tr '\n' ' ')"
echo "lintian: no errors or warnings ($out/lintian.log)"

echo "== install into a clean $image"
docker run --rm --platform linux/amd64 -v "$root/$out:/out:ro" "$image" \
    bash /out/deb-install-check.sh /out >"$out/install.log" 2>&1 || {
    echo "install check failed; $out/install.log:"
    tail -n 30 "$out/install.log"
    exit 1
}
while IFS= read -r line; do echo "$line"; done <"$out/install.log"
echo "== passed"
