# lennys-benchmark packaging

This directory builds native distribution packages for **lennys-benchmark** in three
formats.  Every package ships the same payload:

| file | purpose |
|------|---------|
| `/usr/bin/lennys-benchmark` | the benchmark |
| `/usr/share/applications/lennys-benchmark.desktop` | desktop entry |
| `/usr/share/icons/hicolor/256x256/apps/lennys-benchmark.png` | icon |
| `/usr/share/man/man1/lennys-benchmark.1` | manual page |
| `/usr/share/doc/lennys-benchmark/copyright` | MIT licence |

Packages are always built from a **portable** binary: the build scripts copy
the sources to a scratch directory and compile with `-O2` and no
`-march=native`, so the result runs on any x86-64 CPU.  The in-tree
`make` build still uses `-march=native` for maximum local performance.

## Quick start

```bash
sudo apt install dpkg-dev libepoxy-dev libgl-dev libx11-dev   # build deps
bash packaging/build-all.sh
ls -l packaging/build/
```

Individual formats:

```bash
bash packaging/build-deb.sh       # packaging/build/lennys-benchmark_1.0.0_amd64.deb
bash packaging/build-appimage.sh  # packaging/build/lennys-benchmark-1.0.0-x86_64.AppImage
bash packaging/build-xbps.sh      # packaging/build/lennys-benchmark-1.0.0_1.x86_64.xbps
```

The icon is generated reproducibly with `python3 packaging/make-icon.py`
(requires Pillow); a committed copy lives in `packaging/icon/`.

## How each format is built

### .deb (Debian, Ubuntu, Mint, …)

The package is assembled as a Debian policy compliant tree and packed with
`dpkg-deb`, the documented low-level tool of the dpkg suite (see
`deb-control(5)` and `dpkg-deb(1)`).  The tree contains:

* `DEBIAN/control` — generated from `packaging/deb/control.in`
  (`Package`, `Version`, `Architecture`, `Depends`, `Installed-Size`,
  `Maintainer`, `Homepage`, `Description`).
* `DEBIAN/md5sums` — computed over every installed file.
* `DEBIAN/postinst` / `DEBIAN/postrm` — refresh the desktop and icon caches.
* `/usr/share/doc/lennys-benchmark/copyright` (machine-readable DEP-5) and a gzipped
  Debian `changelog`.

The archive is created with `dpkg-deb --build --root-owner-group`, which sets
`root:root` ownership without requiring root.  Verify with:

```bash
dpkg-deb --info  packaging/build/lennys-benchmark_1.0.0_amd64.deb
dpkg-deb --contents packaging/build/lennys-benchmark_1.0.0_amd64.deb
sudo dpkg -i packaging/build/lennys-benchmark_1.0.0_amd64.deb
```

`dpkg-buildpackage` exists on the build host but the `debhelper` toolchain
does not, so the equivalent packaging steps are performed explicitly by the
script.

### .AppImage (portable, any glibc distribution)

Follows the AppImage/AppDir specification
(<https://docs.appimage.org/reference/appdir.html>):

* `AppRun` — entry point (created by `linuxdeploy`, or the fallback in
  `packaging/appimage/AppRun`).
* one root `.desktop` file, a matching icon and `.DirIcon`.
* payload under `usr/`.
* shared libraries are gathered with `linuxdeploy` (using the AppImage
  excludelist, so host GPU/GL libraries are **not** bundled).  If linuxdeploy
  is unavailable the image is still valid and uses the host libraries.

The single-file image is produced with `appimagetool` (downloaded
automatically).  Run it with:

```bash
chmod +x packaging/build/lennys-benchmark-1.0.0-x86_64.AppImage
./packaging/build/lennys-benchmark-1.0.0-x86_64.AppImage --info
```

### .xbps (Void Linux)

The canonical way to build a Void package is `xbps-src` from
`void-linux/void-packages`, which needs a Void chroot (see the
[Void handbook](https://docs.voidlinux.org/xbps/index.html) and the
[xbps-create(1) man page](https://man.voidlinux.org/xbps-create.1)).  The
reference source template lives in `packaging/xbps/template`.

Because no Void chroot / container runtime is available here, `build-xbps.sh`
performs the same final step directly: it downloads the official `xbps` and
`libxbps` packages from `repo-default.voidlinux.org`, unpacks `xbps-create`
and runs it against a staged tree with the correct metadata
(`--shlib-requires`, dependencies, description, licence).  The output is a
genuine `lennys-benchmark-1.0.0_1.x86_64.xbps`.

Inspect it with (on Void):

```sh
xbps-query -R --property=... ./lennys-benchmark-1.0.0_1.x86_64.xbps
xbps-rindex -a lennys-benchmark-1.0.0_1.x86_64.xbps
```

## Continuous integration

`.github/workflows/release.yml` builds all three packages on every tag and
attaches them to the GitHub release.
