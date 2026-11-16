#!/bin/bash -eu
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ClusterFuzzLite's build step: the fuzz targets, into $OUT.
"$SRC/hackrfduplex/tools/fuzz-build.sh" "$OUT"
