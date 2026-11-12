#!/bin/sh
# Runs the Android unit tests and builds the APK into dist/.
# Runs in docker/android.Dockerfile, or anywhere with JDK 21 and $ANDROID_HOME holding platforms;android-37.0 and
# build-tools;36.0.0.
#
# Signing: with KEYSTORE_FILE (path to a .jks) or KEYSTORE_B64 (its base64), plus KS_PASS, KEY_ALIAS and KEY_PASS, the release
# build is signed -> dist/SCR_Connect-$VERSION-android.apk. Without them the debug build is shipped
# (applicationId io.github.matszwe02.scrconnect.debug, debug key) -> dist/SCR_Connect-$VERSION-android-debug.apk.
set -e
cd "$(dirname "$0")/../.."
VERSION=${VERSION:-dev}
ROOT=$PWD
case "$KEYSTORE_FILE" in ""|/*) ;; *) KEYSTORE_FILE="$ROOT/$KEYSTORE_FILE" ;; esac   # relative to the repository root
mkdir -p dist
cd android

# The APK is packaged from scratch every time: the incremental packaging kept a stale classes3.dex (core classes and the
# webcam plugin of an early build) in every APK since, although the merged dex files were up to date. The rest of the build
# stays incremental.
rm -rf build/outputs/apk build/intermediates/incremental/packageDebug build/intermediates/incremental/packageRelease \
  build/intermediates/apk

# unit tests and the APK in one Gradle run: one JVM start-up and one configuration phase
if [ -n "$KEYSTORE_FILE$KEYSTORE_B64" ]; then
  ./gradlew --no-daemon testDebugUnitTest assembleRelease
  BT="$ANDROID_HOME/build-tools/36.0.0"
  T=$(mktemp -d)
  trap 'rm -rf "$T"' EXIT
  if [ -z "$KEYSTORE_FILE" ]; then
    KEYSTORE_FILE="$T/release.jks"
    echo "$KEYSTORE_B64" | base64 -d > "$KEYSTORE_FILE"
  fi
  unsigned=$(find build/outputs/apk/release -name '*.apk' | head -n1)
  OUT="$ROOT/dist/SCR_Connect-$VERSION-android.apk"
  "$BT/zipalign" -p -f 4 "$unsigned" "$T/aligned.apk"
  "$BT/apksigner" sign --ks "$KEYSTORE_FILE" --ks-pass env:KS_PASS --ks-key-alias "$KEY_ALIAS" --key-pass env:KEY_PASS \
    --out "$OUT" "$T/aligned.apk"
  "$BT/apksigner" verify "$OUT"
else
  echo "No signing key configured: building the debug APK" >&2
  [ -n "$GITHUB_ACTIONS" ] && echo "::warning::No signing secrets configured: shipping the debug build"
  ./gradlew --no-daemon testDebugUnitTest assembleDebug
  OUT="$ROOT/dist/SCR_Connect-$VERSION-android-debug.apk"
  cp "$(find build/outputs/apk/debug -name '*.apk' | head -n1)" "$OUT"
fi
ls -lh "$OUT"
