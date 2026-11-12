# Android toolchain: JDK 21 and the SDK packages the app compiles against. Builds the APK.
# Google's command-line tools come from Ubuntu's installer package, which checks the download against the checksum it carries;
# sdkmanager then checks every SDK package against Google's repository checksums.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive \
    ANDROID_HOME=/opt/android-sdk
RUN echo 'google-android-licenses google-android-licenses/accepted-android-sdk-license boolean true' | debconf-set-selections \
 && apt-get update \
 && apt-get install -y --no-install-recommends openjdk-21-jdk-headless ca-certificates google-android-cmdline-tools-13.0-installer \
 && SDKM=$(find /usr/lib/android-sdk/cmdline-tools -path '*/bin/sdkmanager' | head -n1) \
 && yes | "$SDKM" --sdk_root="$ANDROID_HOME" --licenses >/dev/null \
 && "$SDKM" --sdk_root="$ANDROID_HOME" "platform-tools" "platforms;android-37.0" "build-tools;36.0.0" \
 && apt-get purge -y --auto-remove google-android-cmdline-tools-13.0-installer google-android-build-tools 'google-android-*' \
 && rm -rf /var/lib/apt/lists/* /usr/lib/android-sdk \
 && chmod -R a+rwX "$ANDROID_HOME"
