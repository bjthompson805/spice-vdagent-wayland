#!/bin/bash
# Build the .deb inside a debian container from THIS checkout. dpkg-buildpackage
# builds in place from a source tree with debian/ at its root -- unlike the
# Arch and RPM builds, no separate tarball is needed (debian/source/format is
# "3.0 (native)", so there's no upstream-orig-tarball requirement either).
set -euo pipefail
version="${1:?usage: build-deb.sh <version> <out-dir>}"
out="${2:?usage: build-deb.sh <version> <out-dir>}"
repo="$(pwd)"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
	build-essential devscripts debhelper dh-autoreconf pkgconf \
	libgtk-4-dev libglib2.0-dev libwayland-dev libxfixes-dev libxrandr-dev \
	libxinerama-dev libx11-dev libasound2-dev libdbus-1-dev libdrm-dev \
	libpciaccess-dev libsystemd-dev spice-protocol

build_root=/tmp/debbuild/spice-vdagent-wayland
mkdir -p "$build_root"
cp -a "$repo"/. "$build_root/"
rm -rf "$build_root/packaging"
cp -r "$repo/packaging/debian" "$build_root/debian"
sed -i "0,/^spice-vdagent-wayland (.*)/s//spice-vdagent-wayland ($version-1)/" \
	"$build_root/debian/changelog"

cd "$build_root"
dpkg-buildpackage -us -uc -b

mkdir -p "$out"
cp /tmp/debbuild/*.deb "$out/"
