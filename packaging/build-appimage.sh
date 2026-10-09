#!/usr/bin/env bash
#
# Build a portable AppImage for lennys-benchmark.
#
# AppDir layout follows the AppImage/AppDir specification: AppRun, a single
# root .desktop file, a matching icon and .DirIcon, plus the payload under
# usr/.  Shared libraries are gathered with linuxdeploy (when available); if
# that fails we still produce a valid AppImage that uses the host libraries.
# The final single-file image is created with appimagetool.
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=common.sh
source "$here/common.sh"

root="$(pk_repo_root)"
build="$root/packaging/build"
work="$build/appimage-work"
appdir="$build/AppDir"
tools="$build/tools"

echo "==> building portable binary"
pk_build_portable "$root" "$work"

echo "==> staging AppDir"
rm -rf "$appdir"
install -d "$appdir/usr/bin" \
           "$appdir/usr/share/applications" \
           "$appdir/usr/share/icons/hicolor/256x256/apps" \
           "$appdir/usr/share/man/man1" \
           "$appdir/usr/share/doc/$PKG_NAME"

install -m 0755 "$work/bin/lennys-benchmark" "$appdir/usr/bin/lennys-benchmark"

install -m 0644 "$here/lennys-benchmark.desktop" "$appdir/usr/share/applications/lennys-benchmark.desktop"
install -m 0644 "$here/icon/lennys-benchmark.png"  "$appdir/usr/share/icons/hicolor/256x256/apps/lennys-benchmark.png"
install -m 0644 "$here/lennys-benchmark.1"          "$appdir/usr/share/man/man1/lennys-benchmark.1"
install -m 0644 "$root/LICENSE"             "$appdir/usr/share/doc/$PKG_NAME/copyright"

# Root-level metadata required by appimagetool.
ln -sf "usr/share/applications/lennys-benchmark.desktop" "$appdir/lennys-benchmark.desktop"
ln -sf "usr/share/icons/hicolor/256x256/apps/lennys-benchmark.png" "$appdir/lennys-benchmark.png"
ln -sf "lennys-benchmark.png" "$appdir/.DirIcon"

echo "==> deploying shared libraries"
if command -v linuxdeploy >/dev/null 2>&1; then
    if linuxdeploy --appdir "$appdir" \
            --executable "$appdir/usr/bin/lennys-benchmark" \
            --desktop-file "$appdir/usr/share/applications/lennys-benchmark.desktop" \
            --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/lennys-benchmark.png"; then
        echo "    linuxdeploy finished"
    else
        echo "    WARNING: linuxdeploy failed; continuing without bundled libraries" >&2
    fi
else
    echo "    linuxdeploy not found; using host libraries"
fi

# Guarantee an AppRun exists (linuxdeploy normally provides one).
if [ ! -e "$appdir/AppRun" ]; then
    install -m 0755 "$here/appimage/AppRun" "$appdir/AppRun"
fi

echo "==> fetching appimagetool"
tool="$build/tools/appimagetool"
if [ ! -x "$tool" ]; then
    mkdir -p "$build/tools"
    curl -L --fail --retry 3 -o "$tool" \
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
    chmod +x "$tool"
fi

out="$build/${PKG_NAME}-${PKG_VERSION}-x86_64.AppImage"
rm -f "$out"
echo "==> packing $out"
ARCH=x86_64 "$tool" --no-appstream "$appdir" "$out"
chmod +x "$out"

echo
echo "OK: $out"
