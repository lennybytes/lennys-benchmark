#!/usr/bin/env bash
#
# Shared variables and helpers for the lennys-benchmark packaging scripts.
# Sourced by build-deb.sh, build-appimage.sh, build-xbps.sh and build-all.sh.
#
set -euo pipefail

PKG_NAME="lennys-benchmark"
PKG_VERSION="1.0.0"
PKG_REVISION="1"
PKG_MAINTAINER="lfisbeck650-cloud <lfisbeck650-cloud@users.noreply.github.com>"
PKG_HOMEPAGE="https://github.com/lfisbeck650-cloud/lennys-benchmark"
PKG_LICENSE="MIT"
PKG_DESC="CPU + GPU benchmark with OpenGL rendering"
PKG_ARCH_DEB="amd64"
PKG_ARCH_XBPS="x86_64"

# Repository root (parent of this packaging directory).
pk_repo_root() {
    ( cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd )
}

# Copy the sources into a scratch directory and build a *portable* binary
# there (no -march=native), so the resulting packages run on any x86-64 CPU.
#   $1 = repository root
#   $2 = scratch directory that will receive src/, Makefile, scripts/ and bin/
pk_build_portable() {
    local root="$1" dest="$2"
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -a "$root/src" "$root/Makefile" "$dest/"
    make -C "$dest" clean >/dev/null 2>&1 || true
    make -C "$dest" -j"$(nproc)" \
        CXXFLAGS="-O2 -std=c++20 -Wall -Wextra -Wno-unused-parameter -pthread -Isrc"
}

# Human readable installed size in KiB for DEBIAN/control.
pk_dir_size_kib() {
    du -sk "$1" | cut -f1
}
