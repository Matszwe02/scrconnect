#!/bin/sh
# Builds SCR Connect in Docker; the host needs only Docker. Outputs go to dist/, replacing older builds of the same kind.
#
#   docker/build.sh                 everything: desktop (tarball), appimage, flatpak, apk
#   docker/build.sh desktop apk     only these targets
#
# Targets:
#   desktop   compile the desktop app, make the bundle scrconnect-linux-unpacked/ and the tarball
#   appimage  the AppImage   (builds the desktop first if scrconnect-linux-unpacked/ is missing)
#   flatpak   the Flatpak    (likewise; runs a privileged container, as flatpak-builder needs one)
#   apk       the Android APK, debug-signed unless signing variables are set (see docker/scripts/apk.sh)
#
# Environment: VERSION (default dev-<commit>) names the files; KEYSTORE_FILE (relative to the repository root),
# KS_PASS, KEY_ALIAS, KEY_PASS sign the APK. Caches (ccache, Gradle, appimagetool) live in .cache/.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
VERSION=${VERSION:-dev-$(git rev-parse --short=7 HEAD 2>/dev/null || echo local)}
export VERSION
FLATPAK_IMAGE=ghcr.io/flathub-infra/flatpak-github-actions:freedesktop-24.08
mkdir -p .cache/home dist

image() { # image <name>: build docker/<name>.Dockerfile as scrconnect-<name>-build
  docker build -t "scrconnect-$1-build" -f "docker/$1.Dockerfile" docker
}
run() { # run <image> <command...>: as the calling user, with the repository at /src
  img=$1; shift
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/src/.cache/home -e VERSION \
    -e CCACHE_DIR=/src/.cache/ccache -e GRADLE_USER_HOME=/src/.cache/gradle -e ANDROID_USER_HOME=/src/.cache/home/.android \
    -e CCACHE_MAXSIZE -e GITHUB_ACTIONS -e APPIMAGETOOL -e KEYSTORE_FILE -e KEYSTORE_B64 -e KS_PASS -e KEY_ALIAS -e KEY_PASS \
    -v "$ROOT:/src" -w /src "$img" "$@"
}

desktop() { image desktop; run scrconnect-desktop-build sh docker/scripts/desktop.sh; }
need_bundle() { [ -d scrconnect-linux-unpacked ] || desktop; }
appimage() { need_bundle; image desktop; run scrconnect-desktop-build sh docker/scripts/appimage.sh; }
flatpak() {
  need_bundle
  docker run --rm --privileged -e VERSION -e OWNER="$(id -u):$(id -g)" -v "$ROOT:/src" -w /src "$FLATPAK_IMAGE" \
    sh docker/scripts/flatpak.sh
}
apk() { image android; run scrconnect-android-build sh docker/scripts/apk.sh; }

# A new build replaces the previous ones of its kind in dist/, which is meant to hold one current set
prune() { # prune <target>
  case "$1" in
    desktop) pattern='scrconnect-*-linux-x86_64.tar.xz' ;;
    appimage) pattern='SCR_Connect-*-x86_64.AppImage' ;;
    flatpak) pattern='SCR_Connect-*-x86_64.flatpak' ;;
    apk) pattern='SCR_Connect-*-android*.apk' ;;
  esac
  for f in dist/$pattern; do
    case "$f" in *"-$VERSION-"*) ;; *) [ -e "$f" ] && rm -f -- "$f" ;; esac
  done
}

targets=${*:-desktop appimage flatpak apk}
[ "$targets" = all ] && targets="desktop appimage flatpak apk"
for t in $targets; do
  case "$t" in
    desktop|appimage|flatpak|apk) echo "==> $t"; "$t"; prune "$t" ;;
    *) echo "unknown target: $t (desktop, appimage, flatpak, apk, all)" >&2; exit 2 ;;
  esac
done

cp tools/reload-plugins.sh dist/ # installs the build next to it and reloads both sides (see dist/README.txt)
(cd dist && rm -f SHA256SUMS && sha256sum -- *.apk *.AppImage *.flatpak *.tar.xz > SHA256SUMS 2>/dev/null || true)
ls -lh dist
