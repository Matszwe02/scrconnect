#!/bin/sh
# Compiles kdeconnect-kde (with the mirroring plugins), installs it into AppDir/, turns that into the self-contained bundle
# scrconnect-linux-unpacked/ and packs it as dist/scrconnect-$VERSION-linux-x86_64.tar.xz.
# Runs in docker/desktop.Dockerfile (or any Ubuntu 24.04 + KDE neon with the same packages). Uses ccache when installed.
set -e
cd "$(dirname "$0")/../.."
VERSION=${VERSION:-dev}

launcher=
command -v ccache >/dev/null && launcher="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache"
# shellcheck disable=SC2086
cmake -S desktop -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF $launcher
ninja -C build
rm -rf AppDir
DESTDIR="$PWD/AppDir" ninja -C build install
command -v ccache >/dev/null && ccache -s

sh docker/packaging/bundle.sh AppDir scrconnect-linux-unpacked
# license texts and notices travel inside every desktop package
L=scrconnect-linux-unpacked/licenses
mkdir -p "$L"
cp scrcpy/LICENSE "$L/scrcpy-LICENSE-Apache-2.0.txt"
cp -r desktop/LICENSES "$L/kdeconnect-kde-LICENSES"

mkdir -p dist
tar -I 'xz -T0 -6' -cf "dist/scrconnect-$VERSION-linux-x86_64.tar.xz" scrconnect-linux-unpacked
ls -lh "dist/scrconnect-$VERSION-linux-x86_64.tar.xz"
