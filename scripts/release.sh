#!/bin/bash
# Build a signed + notarized release of twinc.
#
#   scripts/release.sh            (version taken from src/common.h)
#
# Needs, once:
#   * "Developer ID Application" and "Developer ID Installer" certificates in the keychain
#     (Xcode → Settings → Accounts → Manage Certificates → +)
#   * stored notarization credentials:
#       xcrun notarytool store-credentials twinc-notary --apple-id <apple-id> --team-id <TEAMID>
#
# Produces in dist/:
#   twinc-<v>-macos.zip   signed + notarized universal binary, ncm.sh, ncm.zsh, README, LICENSE
#   twinc-<v>.pkg         signed + notarized + stapled installer (/usr/local/bin/twinc)
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define TWINC_VERSION "\(.*\)"/\1/p' src/common.h)
PROFILE=${NOTARY_PROFILE:-twinc-notary}
BUNDLE_ID=org.vanderheiden.twinc

APP_ID=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)
INST_ID=$(security find-identity -v | sed -n 's/.*"\(Developer ID Installer: [^"]*\)".*/\1/p' | head -1)
if [[ -z "$APP_ID" ]]; then
  echo "error: no 'Developer ID Application' certificate found in the keychain" >&2
  exit 1
fi
echo "==> twinc $VERSION"
echo "    signing as: $APP_ID"
[[ -n "$INST_ID" ]] && echo "    pkg signed as: $INST_ID" || echo "    (no Developer ID Installer certificate: skipping .pkg)"

notarize() {  # $1 = file
  echo "==> notarizing $(basename "$1") (takes a few minutes)"
  local out id
  out=$(xcrun notarytool submit "$1" --keychain-profile "$PROFILE" --wait 2>&1) || true
  echo "$out" | sed 's/^/    /'
  if ! echo "$out" | grep -q "status: Accepted"; then
    id=$(echo "$out" | sed -n 's/^ *id: \(.*\)/\1/p' | head -1)
    [[ -n "$id" ]] && xcrun notarytool log "$id" --keychain-profile "$PROFILE" || true
    echo "error: notarization failed" >&2
    exit 1
  fi
}

DIST=dist
rm -rf "$DIST"
mkdir -p "$DIST"

echo "==> building universal binary"
make universal >/dev/null
lipo -archs twinc

echo "==> signing"
codesign --force --options runtime --timestamp --identifier "$BUNDLE_ID" --sign "$APP_ID" twinc
codesign --verify --strict --verbose=2 twinc

# ---- zip with the bare binary
STAGE="$DIST/twinc-$VERSION"
mkdir -p "$STAGE"
cp twinc ncm.sh ncm.zsh README.md LICENSE "$STAGE/"
ZIP="$DIST/twinc-$VERSION-macos.zip"
ditto -c -k --keepParent "$STAGE" "$ZIP"
notarize "$ZIP"

# ---- installer package
if [[ -n "$INST_ID" ]]; then
  ROOT="$DIST/pkgroot"
  install -d "$ROOT/usr/local/bin" "$ROOT/usr/local/share/twinc"
  install -m 755 twinc "$ROOT/usr/local/bin/twinc"
  install -m 644 ncm.sh ncm.zsh "$ROOT/usr/local/share/twinc/"
  pkgbuild --root "$ROOT" --identifier "$BUNDLE_ID" --version "$VERSION" --install-location / \
    "$DIST/unsigned.pkg" >/dev/null
  PKG="$DIST/twinc-$VERSION.pkg"
  productsign --sign "$INST_ID" --timestamp "$DIST/unsigned.pkg" "$PKG" >/dev/null
  rm -f "$DIST/unsigned.pkg"
  rm -rf "$ROOT"
  notarize "$PKG"
  xcrun stapler staple "$PKG"
  spctl --assess --type install -vv "$PKG" || true
fi

echo "==> done"
(cd "$DIST" && shasum -a 256 ./*.zip ./*.pkg 2>/dev/null)
