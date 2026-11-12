#!/bin/sh
# Turns an installed kdeconnect-kde tree into a self-contained bundle: the tree itself plus every Qt/KF6 library, Qt plugin and
# QML module it needs. The result runs on any x86_64 Linux with glibc >= the build host's (Ubuntu 24.04 -> 2.39) without
# Qt6/KF6 installed. It is the common input of the unpacked tarball, the AppImage and the Flatpak.
#
# usage: bundle.sh <install tree (DESTDIR, contains usr/)> <output dir>      Run it on the machine that built the tree.
# Launch the result with <output>/run.sh <binary> [args] or <output>/scrconnect.
set -e
HERE=$(dirname "$(readlink -f "$0")")
APPDIR=$(readlink -f "$1")
mkdir -p "$2"; N=$(readlink -f "$2")
rm -rf "$N"; mkdir -p "$N/lib" "$N/qt6" "$N/usr" "$N/share"
M=/usr/lib/x86_64-linux-gnu

cp -a "$APPDIR/usr/." "$N/usr/"
# Qt plugins, QML modules and KDE framework plugins the app may load: the system's, then the ones installed with the fork
cp -a "$M/qt6/plugins" "$N/qt6/"
cp -a "$M/qt6/qml" "$N/qt6/"
[ -d "$N/usr/lib/x86_64-linux-gnu/qt6/plugins" ] && cp -a "$N/usr/lib/x86_64-linux-gnu/qt6/plugins/." "$N/qt6/plugins/"
[ -d "$N/usr/lib/x86_64-linux-gnu/qt6/qml" ] && cp -a "$N/usr/lib/x86_64-linux-gnu/qt6/qml/." "$N/qt6/qml/"
[ -d "$APPDIR/etc" ] && cp -a "$APPDIR/etc" "$N/etc"
# PipeWire client modules and SPA plugins are loaded at run time (webcam helper PipeWire backend)
for d in spa-0.2 pipewire-0.3; do [ -d "$M/$d" ] && cp -a "$M/$d" "$N/lib/$d"; done
[ -d /usr/share/icons ] && cp -a /usr/share/icons "$N/share/"

# closure of shared library dependencies of every ELF file in the bundle
ELFS=$(mktemp); LIBS=$(mktemp)
find "$N" -type f \( -name '*.so*' -o -perm -u+x \) | while read -r f; do file -b "$f" | grep -q '^ELF' && echo "$f"; done > "$ELFS"
while read -r f; do LD_LIBRARY_PATH="$N/usr/lib/x86_64-linux-gnu:$M" ldd "$f" 2>/dev/null; done < "$ELFS" | awk '/=> \//{print $3}' | sort -u > "$LIBS"
# not bundled: libc and friends (must match the host kernel/loader) and the GPU stack (must match the host driver)
grep -vE '/(libc|libm|libdl|libpthread|librt|libresolv|libutil|libnsl|libanl)\.so|ld-linux|/libGL|/libEGL|/libGLX|/libGLdispatch|/libOpenGL|/libgbm|/libdrm|/libvulkan|/libwayland-(egl|server)|libnvidia|/libstdc\+\+' "$LIBS" \
  | while read -r l; do cp --update=none -L "$l" "$N/lib/"; done
cp --update=none -L "$M/libstdc++.so.6" "$N/lib/"
rm -f "$ELFS" "$LIBS"

# Stand-ins for NVIDIA's CUDA and video libraries: empty, so FFmpeg and Qt find no CUDA, NVDEC or NVENC (see run.sh)
mkdir -p "$N/lib/nogpu"
for l in libcuda.so.1 libnvcuvid.so.1 libnvidia-encode.so.1 libnvidia-ml.so.1; do
  printf '' | cc -shared -x c - -o "$N/lib/nogpu/$l"
done

cat > "$N/run.sh" <<'R'
#!/bin/sh
# usage: run.sh <kdeconnect-app|kdeconnectd|kdeconnect-mirror|kdeconnect-cli|...> [args]
H=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$H/lib:$H/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# No GPU: nothing here needs one (the video is decoded in software), and NVIDIA's driver could leave the daemon stuck in
# the kernel for good, unkillable, still holding the screen edge and the network port. SCRCONNECT_GPU=1 allows it again.
if [ "${SCRCONNECT_GPU:-}" != 1 ]; then
  export LD_LIBRARY_PATH="$H/lib/nogpu:$LD_LIBRARY_PATH"   # empty libcuda, libnvcuvid...: no CUDA, NVDEC, NVENC
  export QT_XCB_GL_INTEGRATION=none QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1
  export __GLX_VENDOR_LIBRARY_NAME=mesa VK_LOADER_DRIVERS_DISABLE='*nvidia*'
  export VDPAU_DRIVER=none LIBVA_DRIVER_NAME=none
  for f in /usr/share/glvnd/egl_vendor.d/50_mesa.json /usr/lib/x86_64-linux-gnu/GL/default/share/glvnd/egl_vendor.d/50_mesa.json; do
    [ -e "$f" ] && { export __EGL_VENDOR_LIBRARY_FILENAMES="$f"; break; }
  done
fi
export SPA_PLUGIN_DIR="$H/lib/spa-0.2" PIPEWIRE_MODULE_DIR="$H/lib/pipewire-0.3"
export QT_PLUGIN_PATH="$H/qt6/plugins" QML_IMPORT_PATH="$H/qt6/qml" QML2_IMPORT_PATH="$H/qt6/qml"
export QT_QPA_PLATFORM_PLUGIN_PATH="$H/qt6/plugins/platforms"
export XDG_DATA_DIRS="$H/usr/share:$H/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
b=$1; shift
exec "$H/usr/bin/$b" "$@"
R
cp "$HERE/scrconnect" "$N/scrconnect"
chmod +x "$N/run.sh" "$N/scrconnect"
du -sh "$N"
