#!/bin/sh
# Packs scrconnect-linux-unpacked/ (from desktop.sh) as dist/SCR_Connect-$VERSION-x86_64.AppImage and smoke-tests it.
# Uses $APPIMAGETOOL if set, else downloads the pinned appimagetool into .cache/ and checks its SHA-256.
set -e
cd "$(dirname "$0")/../.."
VERSION=${VERSION:-dev}
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
export APPIMAGE_EXTRACT_AND_RUN=1   # no FUSE needed (containers, CI)

[ -d scrconnect-linux-unpacked ] || { echo "scrconnect-linux-unpacked/ is missing: build the desktop first" >&2; exit 1; }
if [ -z "$APPIMAGETOOL" ]; then
  APPIMAGETOOL=.cache/appimagetool-1.9.1
  if ! echo "$APPIMAGETOOL_SHA256  $APPIMAGETOOL" | sha256sum -c - >/dev/null 2>&1; then
    mkdir -p .cache
    curl -fsSL --retry 5 -o "$APPIMAGETOOL" "$APPIMAGETOOL_URL"
    echo "$APPIMAGETOOL_SHA256  $APPIMAGETOOL" | sha256sum -c -
  fi
  chmod +x "$APPIMAGETOOL"
fi

P=docker/packaging/appimage
rm -rf AppImageDir
cp -a scrconnect-linux-unpacked AppImageDir
cp "$P/AppRun" "$P/scrconnect.desktop" AppImageDir/
cp AppImageDir/usr/share/icons/hicolor/scalable/apps/kdeconnect.svg AppImageDir/kdeconnect.svg
chmod +x AppImageDir/AppRun
mkdir -p dist
OUT="dist/SCR_Connect-$VERSION-x86_64.AppImage"
ARCH=x86_64 "$APPIMAGETOOL" AppImageDir "$OUT"
rm -rf AppImageDir

out=$(QT_QPA_PLATFORM=offscreen timeout 60 "$OUT" run kdeconnect-cli --version 2>&1 </dev/null) || true
echo "smoke test: $out"
echo "$out" | grep -qi 'kdeconnect-cli'
ls -lh "$OUT"
