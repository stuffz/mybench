#!/bin/sh
# Assembles the .deb inside the mybench-qt-deb container (Ubuntu 24.04).
# Expects the repo mounted at /work with gui/build-deb/mybench and
# gui/build-deb/mybench-backend already built, and $VERSION set to the
# release version in pkgver-pkgrel form (e.g. 0.1.0-10).
set -eu

root=/tmp/pkgroot
rm -rf "$root"

install -Dm755 /work/gui/build-deb/mybench "$root/usr/bin/mybench"
install -Dm755 /work/gui/build-deb/mybench-backend "$root/usr/bin/mybench-backend"
install -Dm644 /work/packaging/linux/mybench.desktop \
    "$root/usr/share/applications/mybench.desktop"
install -Dm644 /work/build/appicon.png \
    "$root/usr/share/icons/hicolor/512x512/apps/mybench.png"

# dpkg-shlibdeps derives Depends from what the GUI binary actually links —
# the qt6 runtime package names differ per Ubuntu release (t64 suffixes), so
# hardcoding them would rot. It insists on a debian/control existing.
# The static Go backend adds no dependencies.
mkdir -p /tmp/shlibs/debian
cd /tmp/shlibs
touch debian/control
depends=$(dpkg-shlibdeps -O "$root/usr/bin/mybench" 2>/dev/null | sed 's/^shlibs:Depends=//')

mkdir -p "$root/DEBIAN"
sed -e "s/@VERSION@/$VERSION/" -e "s/@DEPENDS@/$depends/" \
    /work/packaging/deb/control.in > "$root/DEBIAN/control"

dpkg-deb --build --root-owner-group "$root" \
    "/work/bin/mybench_${VERSION}_amd64.deb"
