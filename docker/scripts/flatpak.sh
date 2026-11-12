#!/bin/sh
# Packs scrconnect-linux-unpacked/ (from desktop.sh) as dist/SCR_Connect-$VERSION-x86_64.flatpak.
# Runs in ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-24.08 (privileged), or anywhere with flatpak-builder and
# org.freedesktop.Platform//24.08 + org.freedesktop.Sdk//24.08 installed.
# OWNER=uid:gid hands the outputs back to that user (the container runs as root).
set -e
cd "$(dirname "$0")/../.."
VERSION=${VERSION:-dev}

[ -d scrconnect-linux-unpacked ] || { echo "scrconnect-linux-unpacked/ is missing: build the desktop first" >&2; exit 1; }
W=flatpak-build
rm -rf "$W"
mkdir -p "$W" dist
cp -a scrconnect-linux-unpacked "$W/bundle"
cp docker/packaging/flatpak/local.scrconnect.App.yml "$W/"
OUT="dist/SCR_Connect-$VERSION-x86_64.flatpak"
(
  cd "$W"
  flatpak-builder --force-clean --disable-cache --disable-rofiles-fuse --repo=repo build-dir local.scrconnect.App.yml
  flatpak build-bundle repo "../$OUT" local.scrconnect.App
)
rm -rf "$W"
[ -n "$OWNER" ] && chown "$OWNER" dist "$OUT"
ls -lh "$OUT"
