#!/bin/bash
# Build the Arch package inside an archlinux container from THIS checkout,
# not from the committed PKGBUILD's own git+<url>#tag=... source array --
# that would re-fetch the same tag from GitHub redundantly, and would fail
# outright for a test run against a tag that isn't pushed yet. CI swaps in a
# local tarball instead; everything else in the PKGBUILD is unchanged, so
# what CI verifies is the same build()/package() logic a real user's makepkg
# run would execute.
set -euo pipefail
version="${1:?usage: build-arch.sh <version> <out-dir>}"
out="${2:?usage: build-arch.sh <version> <out-dir>}"
repo="$(pwd)"

pacman -Syu --noconfirm
pacman -S --noconfirm --needed base-devel git \
	gtk4 systemd-libs wayland libxfixes libxrandr libxinerama libx11 \
	alsa-lib dbus libdrm glib2 libpciaccess spice-protocol

build_root=/tmp/pkgbuild
mkdir -p "$build_root/spice-vdagent-wayland"
cp -a "$repo"/. "$build_root/spice-vdagent-wayland/"
rm -rf "$build_root/spice-vdagent-wayland/packaging" "$build_root/spice-vdagent-wayland/.git"
tar -C "$build_root" -czf "$build_root/spice-vdagent-wayland.tar.gz" spice-vdagent-wayland

cp "$repo/packaging/arch/PKGBUILD" "$build_root/PKGBUILD"
sed -i \
	-e "s|^pkgver=.*|pkgver=$version|" \
	-e 's|^source=.*|source=("spice-vdagent-wayland.tar.gz")|' \
	-e '/^sha256sums=/d' \
	-e '/^pkgver() {/,/^}/d' \
	"$build_root/PKGBUILD"
echo 'sha256sums=("SKIP")' >>"$build_root/PKGBUILD"

# makepkg refuses to run as root.
useradd -m builder
chown -R builder:builder "$build_root"
su builder -c "cd '$build_root' && makepkg --noconfirm --syncdeps"

mkdir -p "$out"
cp "$build_root"/*.pkg.tar.zst "$out/"
