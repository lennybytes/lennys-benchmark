#!/usr/bin/env bash
#
# Build a Debian binary package (.deb) for lennys-benchmark.
#
# The package is assembled in a staging tree that follows the Debian policy
# (DEBIAN/control, DEBIAN/md5sums, maintainer scripts, /usr/bin, man page,
# desktop file, icon, documentation) and is then packed with dpkg-deb, the
# documented low-level tool from the dpkg suite.  The binary is rebuilt with
# portable compiler flags (no -march=native) so the package runs on any
# x86-64 CPU.
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=common.sh
source "$here/common.sh"

root="$(pk_repo_root)"
build="$root/packaging/build"
work="$build/deb-work"
stage="$build/deb/$PKG_NAME"

echo "==> building portable binary"
pk_build_portable "$root" "$work"

echo "==> staging filesystem tree"
rm -rf "$stage"
install -d "$stage/DEBIAN" \
           "$stage/usr/bin" \
           "$stage/usr/share/applications" \
           "$stage/usr/share/icons/hicolor/256x256/apps" \
           "$stage/usr/share/man/man1" \
           "$stage/usr/share/doc/$PKG_NAME"

install -m 0755 "$work/bin/lennys-benchmark" "$stage/usr/bin/lennys-benchmark"

install -m 0644 "$root/packaging/lennys-benchmark.desktop" \
    "$stage/usr/share/applications/lennys-benchmark.desktop"
install -m 0644 "$root/packaging/icon/lennys-benchmark.png" \
    "$stage/usr/share/icons/hicolor/256x256/apps/lennys-benchmark.png"
install -m 0644 "$root/packaging/lennys-benchmark.1" \
    "$stage/usr/share/man/man1/lennys-benchmark.1"
gzip -9nf "$stage/usr/share/man/man1/lennys-benchmark.1"

install -m 0644 "$root/packaging/deb/copyright" \
    "$stage/usr/share/doc/$PKG_NAME/copyright"
install -m 0644 "$root/packaging/deb/changelog" \
    "$stage/usr/share/doc/$PKG_NAME/changelog"
gzip -9nf "$stage/usr/share/doc/$PKG_NAME/changelog"

install -m 0755 "$root/packaging/deb/postinst" "$stage/DEBIAN/postinst"
install -m 0755 "$root/packaging/deb/postrm"   "$stage/DEBIAN/postrm"

echo "==> writing control file and checksums"
size="$(du -sk --exclude=DEBIAN "$stage" | cut -f1)"
sed -e "s|@VERSION@|$PKG_VERSION|g" \
    -e "s|@ARCH@|$PKG_ARCH_DEB|g" \
    -e "s|@SIZE@|$size|g" \
    -e "s|@MAINTAINER@|$PKG_MAINTAINER|g" \
    -e "s|@HOMEPAGE@|$PKG_HOMEPAGE|g" \
    "$root/packaging/deb/control.in" > "$stage/DEBIAN/control"

( cd "$stage" && find usr -type f -exec md5sum {} + > DEBIAN/md5sums )

out="$build/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH_DEB}.deb"
rm -f "$out"
echo "==> packing $out"
dpkg-deb --build --root-owner-group "$stage" "$out"

echo "==> package info"
dpkg-deb --info "$out" | sed -n '1,20p'
echo
echo "OK: $out"
