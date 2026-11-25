#!/bin/bash
# Build Linux release packages for the current machine's architecture:
#   dist/twinc-<v>-linux-<arch>.tar.gz   binary, ncm.sh, README, LICENSE
#   dist/twinc_<v>_<debarch>.deb         installs /usr/bin/twinc and /usr/share/twinc/ncm.sh
# Needs: g++, make, libarchive-dev, dpkg-deb.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define TWINC_VERSION "\(.*\)"/\1/p' src/common.h)
ARCH=$(uname -m)
case "$ARCH" in
  x86_64) DEBARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64; DEBARCH=arm64 ;;
  *) DEBARCH=$ARCH ;;
esac

rm -rf dist && mkdir -p dist
make clean >/dev/null
make CXXFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter" >/dev/null
strip twinc

# ---- tar.gz
STAGE=dist/twinc-$VERSION-linux-$ARCH
mkdir -p "$STAGE"
cp twinc ncm.sh README.md LICENSE "$STAGE/"
tar -C dist -czf "$STAGE.tar.gz" "$(basename "$STAGE")"
rm -rf "$STAGE"

# ---- .deb
ROOT=dist/deb
make install DESTDIR="$ROOT" PREFIX=/usr >/dev/null
install -d "$ROOT/usr/share/doc/twinc" "$ROOT/DEBIAN"
install -m 644 README.md "$ROOT/usr/share/doc/twinc/README.md"
install -m 644 LICENSE "$ROOT/usr/share/doc/twinc/copyright"
SIZE=$(du -sk "$ROOT/usr" | cut -f1)
cat > "$ROOT/DEBIAN/control" <<CTRL
Package: twinc
Version: $VERSION
Architecture: $DEBARCH
Maintainer: Walter van der Heiden <walter@vanderheiden.org>
Installed-Size: $SIZE
Depends: libarchive13t64 | libarchive13, libc6, libstdc++6
Section: utils
Priority: optional
Homepage: https://github.com/wvdheiden/twinc
Description: two-panel file manager for the terminal
 twinc is a fast two-panel file manager in the style of the classic DOS
 commanders: blue panels, function-key bar, command line, archives as
 directories, delete to Trash, built-in viewer and editor.
CTRL
dpkg-deb --root-owner-group --build "$ROOT" "dist/twinc_${VERSION}_${DEBARCH}.deb" >/dev/null
rm -rf "$ROOT"

(cd dist && sha256sum ./*)
