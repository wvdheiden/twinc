#!/bin/bash
# After pushing tag v<version> to GitHub: point the Homebrew formula at it.
#   scripts/bump-formula.sh [path-to-homebrew-tap]
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^#define TWINC_VERSION "\(.*\)"/\1/p' src/common.h)
TAP=${1:-../homebrew-tap}
URL="https://github.com/wvdheiden/twinc/archive/refs/tags/v$VERSION.tar.gz"
SHA=$(curl -fsSL "$URL" | shasum -a 256 | cut -d' ' -f1)
F="$TAP/Formula/twinc.rb"
sed -i '' -e "s|^  url \".*\"|  url \"$URL\"|" -e "s|^  sha256 \".*\"|  sha256 \"$SHA\"|" "$F"
echo "updated $F -> v$VERSION ($SHA)"
echo "now: cd $TAP && git commit -am 'twinc $VERSION' && git push"
