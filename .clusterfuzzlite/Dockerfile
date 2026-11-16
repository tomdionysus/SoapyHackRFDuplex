# SPDX-License-Identifier: GPL-3.0-or-later
#
# ClusterFuzzLite's builder for the fuzz targets in fuzz/: the OSS-Fuzz
# base image, pinned, and this tree. The targets need only the module's
# core, so nothing is installed.
FROM gcr.io/oss-fuzz-base/base-builder@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
COPY . $SRC/hackrfduplex
WORKDIR $SRC/hackrfduplex
COPY .clusterfuzzlite/build.sh $SRC/
