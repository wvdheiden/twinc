#!/bin/bash
# Build the Windows release zip (run in an MSYS2 UCRT64 shell):
#   dist/twinc-<v>-windows-x86_64.zip   twinc.exe (static), ncm.ps1, README, LICENSE
# Needs: make mingw-w64-ucrt-x86_64-{gcc,libarchive,pkgconf} zip
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^#define TWINC_VERSION "\(.*\)"/\1/p' src/common.h)
make clean >/dev/null
make -j4 twinc
strip twinc.exe
# only Windows system DLLs may remain
if objdump -p twinc.exe | grep "DLL Name" | grep -viE "kernel32|user32|advapi32|shell32|ole32|bcrypt|crypt32|ws2_32|gdi32|api-ms-win|msvcrt|ucrtbase|xmllite|shlwapi|oleaut32|rpcrt4|ntdll|secur32"; then
  echo "error: unexpected DLL dependency" >&2
  exit 1
fi
rm -rf dist && mkdir -p "dist/twinc-$VERSION"
cp twinc.exe ncm.ps1 README.md LICENSE "dist/twinc-$VERSION/"
(cd dist && zip -qr "twinc-$VERSION-windows-x86_64.zip" "twinc-$VERSION")
rm -rf "dist/twinc-$VERSION"
(cd dist && sha256sum ./*.zip)
