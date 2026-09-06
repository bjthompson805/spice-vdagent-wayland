#!/bin/bash
# Build the RPM inside a fedora container from THIS checkout. Unlike the deb
# build, rpmbuild needs a real source tarball matching %autosetup -n's
# expected directory name (spice-vdagent-wayland-<version>), built here
# instead of relying on GitHub's own archive-by-tag URL naming (which the
# committed spec's Source0 points at for a real `rpmbuild -bs`/mock build --
# not worth depending on for CI, since it'd mean this script does nothing
# but wait on a URL that may not even be live yet for an unpushed test tag).
set -euo pipefail
version="${1:?usage: build-rpm.sh <version> <out-dir>}"
out="${2:?usage: build-rpm.sh <version> <out-dir>}"
repo="$(pwd)"

dnf install -y rpm-build rpmdevtools \
	gcc make autoconf automake libtool \
	'pkgconfig(gtk4)' 'pkgconfig(glib-2.0)' 'pkgconfig(gio-unix-2.0)' \
	'pkgconfig(wayland-client)' 'pkgconfig(xfixes)' 'pkgconfig(xrandr)' \
	'pkgconfig(xinerama)' 'pkgconfig(x11)' 'pkgconfig(alsa)' \
	'pkgconfig(dbus-1)' 'pkgconfig(libdrm)' 'pkgconfig(pciaccess)' \
	'pkgconfig(libsystemd)' 'pkgconfig(spice-protocol)' systemd-rpm-macros

rpmdev-setuptree
topdir="$(rpm --eval '%{_topdir}')"

srcdir="/tmp/rpmsrc/spice-vdagent-wayland-$version"
mkdir -p "$srcdir"
cp -a "$repo"/. "$srcdir/"
rm -rf "$srcdir/packaging" "$srcdir/.git"
tar -C /tmp/rpmsrc -czf "$topdir/SOURCES/spice-vdagent-wayland-$version.tar.gz" \
	"spice-vdagent-wayland-$version"

sed "s|^Version:.*|Version:        $version|" \
	"$repo/packaging/rpm/spice-vdagent-wayland.spec" >"$topdir/SPECS/spice-vdagent-wayland.spec"

rpmbuild -bb "$topdir/SPECS/spice-vdagent-wayland.spec"

mkdir -p "$out"
find "$topdir/RPMS" -name '*.rpm' -exec cp {} "$out/" \;
