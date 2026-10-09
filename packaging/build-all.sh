#!/usr/bin/env bash
#
# Build all lennys-benchmark distribution packages (.deb, .AppImage, .xbps) and write
# a SHA256SUMS file next to them under packaging/build/.
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=common.sh
source "$here/common.sh"

"$here/build-deb.sh"
echo
"$here/build-appimage.sh"
echo
"$here/build-xbps.sh"

root="$(pk_repo_root)"
build="$root/packaging/build"

echo
echo "==> checksums"
(
    cd "$build"
    rm -f SHA256SUMS
    sha256sum \
        "${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH_DEB}.deb" \
        "${PKG_NAME}-${PKG_VERSION}-x86_64.AppImage" \
        "${PKG_NAME}-${PKG_VERSION}_${PKG_REVISION}.${PKG_ARCH_XBPS}.xbps" \
        > SHA256SUMS
    cat SHA256SUMS
)
