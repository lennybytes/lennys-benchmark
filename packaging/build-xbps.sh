#!/usr/bin/env bash
#
# Build a Void Linux binary package (.xbps) for lennys-benchmark.
#
# The canonical way to build .xbps files is void-packages' xbps-src, which
# needs a Void chroot.  On any other distribution the final packaging step
# can be performed directly with xbps-create, the tool xbps-src itself uses.
# This script fetches the official xbps + libxbps binaries from the Void
# repository, unpacks them into packaging/build/tools and runs xbps-create on
# a staged tree.  See packaging/README.md for details.
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=common.sh
source "$here/common.sh"

root="$(pk_repo_root)"
build="$root/packaging/build"
work="$build/xbps-work"
tools="$build/tools/xbps"
stage="$build/xbps/dest"
xbps_version="0.60.7"
xbps_rev="1"
mirror="https://repo-default.voidlinux.org/current"

echo "==> building portable binary"
pk_build_portable "$root" "$work"

echo "==> fetching xbps-create"
if [ ! -x "$tools/root/usr/bin/xbps-create" ]; then
    mkdir -p "$tools/root" "$tools/libroot"
    curl -L --fail --retry 3 -o "$tools/xbps.xbps" \
        "$mirror/xbps-${xbps_version}_${xbps_rev}.x86_64.xbps"
    curl -L --fail --retry 3 -o "$tools/libxbps.xbps" \
        "$mirror/libxbps-${xbps_version}_${xbps_rev}.x86_64.xbps"
    tar --zstd -xf "$tools/xbps.xbps"    -C "$tools/root"
    tar --zstd -xf "$tools/libxbps.xbps" -C "$tools/libroot"
fi
export LD_LIBRARY_PATH="$tools/libroot/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
xbps_create="$tools/root/usr/bin/xbps-create"

echo "==> staging filesystem tree"
rm -rf "$stage"
install -d "$stage/usr/bin" \
           "$stage/usr/share/applications" \
           "$stage/usr/share/icons/hicolor/256x256/apps" \
           "$stage/usr/share/man/man1" \
           "$stage/usr/share/doc/$PKG_NAME"

install -m 0755 "$work/bin/lennys-benchmark" "$stage/usr/bin/lennys-benchmark"

install -m 0644 "$root/packaging/lennys-benchmark.desktop" "$stage/usr/share/applications/lennys-benchmark.desktop"
install -m 0644 "$root/packaging/icon/lennys-benchmark.png"  "$stage/usr/share/icons/hicolor/256x256/apps/lennys-benchmark.png"
install -m 0644 "$root/packaging/lennys-benchmark.1"          "$stage/usr/share/man/man1/lennys-benchmark.1"
install -m 0644 "$root/LICENSE"                       "$stage/usr/share/doc/$PKG_NAME/copyright"

out="$build/${PKG_NAME}-${PKG_VERSION}_${PKG_REVISION}.${PKG_ARCH_XBPS}.xbps"
rm -f "$out"
echo "==> packing $out"
(
    cd "$build"
    "$xbps_create" \
        -A "$PKG_ARCH_XBPS" \
        -n "${PKG_NAME}-${PKG_VERSION}_${PKG_REVISION}" \
        -s "$PKG_DESC" \
        -S "lennys-benchmark benchmarks CPU and GPU performance. The CPU suite runs
multi-threaded integer, FP32, FP64 and memory-bandwidth tests. The GPU suite
renders an OpenGL 4.5 scene with PBR, HDR, bloom and ACES tonemapping and
measures geometry and fill throughput. All GPUs are detected; on hybrid
NVIDIA systems PRIME render offload is used automatically." \
        -H "$PKG_HOMEPAGE" \
        -l "$PKG_LICENSE" \
        -m "$PKG_MAINTAINER" \
        -t "benchmark" \
        -D "libepoxy>=0 libX11>=0 libglvnd>=0 glibc>=2.41_1 gcc-libs>=0" \
        --shlib-requires "libepoxy.so.0 libX11.so.6 libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6" \
        --compression zstd \
        "$stage"
    mv "$(basename "$out")" "$out" 2>/dev/null || true
)

echo
echo "OK: $out"
