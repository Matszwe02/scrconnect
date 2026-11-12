# Desktop toolchain: Ubuntu 24.04 + Qt 6/KF6 from KDE neon. Builds kdeconnect-kde, the bundle, the tarball and the AppImage.
# Ubuntu 24.04 fixes the oldest glibc the desktop packages run on (2.39).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y -o Acquire::Retries=5 curl gnupg ca-certificates file git cmake ninja-build g++ pkg-config patchelf \
      desktop-file-utils squashfs-tools python3 xz-utils zstd ccache \
 && curl -fsSL --retry 5 https://archive.neon.kde.org/public.key | gpg --dearmor -o /usr/share/keyrings/neon.gpg \
 && echo "deb [signed-by=/usr/share/keyrings/neon.gpg] https://archive.neon.kde.org/user noble main" > /etc/apt/sources.list.d/neon.list \
 && echo "deb-src [signed-by=/usr/share/keyrings/neon.gpg] https://archive.neon.kde.org/user noble main" >> /etc/apt/sources.list.d/neon.list \
 && printf 'Package: *\nPin: origin archive.neon.kde.org\nPin-Priority: 900\n' > /etc/apt/preferences.d/neon \
 && apt-get update \
 && apt-get build-dep -y -o Acquire::Retries=5 kdeconnect \
 && apt-get install -y -o Acquire::Retries=5 libavcodec-dev libavutil-dev libswscale-dev qt6-base-private-dev libkf6i18n-dev \
      libpipewire-0.3-dev libpipewire-0.3-modules libspa-0.2-dev \
      libxcb1-dev libxcb-xfixes0-dev libxcb-xinput-dev libxcb-randr0-dev libxkbcommon-x11-dev \
 && rm -rf /var/lib/apt/lists/*
