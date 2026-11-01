#!/usr/bin/env bash
# The Linux release (x86_64), built on Linux: the program (CMake + Ninja, Qt 6.8), an AppDir with Qt bundled by
# linuxdeploy, then into dist/:
#   HairHarpy-<ver>-x86_64.AppImage       — one file: chmod +x and run
#   HairHarpy-<ver>-linux-x86_64.tar.gz   — the same program as a folder, with install.sh (menu entry, .hairmesh files)
#   SHA256SUMS-linux.txt
# Usage: bash installer/linux/build-linux.sh [--qt <Qt folder, e.g. ~/Qt/6.8.3/gcc_64>]
# Needs: cmake 3.24+, ninja, g++ 11+, git, glslang-tools, curl, the libraries Qt's xcb plugin needs (BUILDING.md lists
# them). .github/workflows/linux.yml runs it on GitHub for every release tag.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ver=$(head -n 1 "$root/installer/version.txt" | tr -d '\r[:space:]')
qt=${QT_ROOT_DIR:-}   # (install-qt-action sets it on GitHub)
while [ $# -gt 0 ]; do case $1 in --qt) qt=$2; shift 2 ;; *) echo "unknown option: $1"; exit 2 ;; esac; done
[ -n "$qt" ] && [ -x "$qt/bin/qmake" ] || { echo "Qt not found: pass --qt <folder with bin/qmake>, e.g. ~/Qt/6.8.3/gcc_64"; exit 2; }

bld=$root/build/Linux; app=$root/build/AppDir; tools=$root/build/linux-tools; dist=$root/dist
cmake -S "$root" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$qt" -DHM_DEVTOOLS=ON
cmake --build "$bld" --target HairHarpy -- -k 0   # (-k 0: on past a failed file — every error in one run)

# linuxdeploy + its Qt plugin (they run without FUSE: extracted on the fly)
mkdir -p "$tools"
for t in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
         linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
  f=$tools/$(basename "$t"); [ -s "$f" ] || curl -fL --retry 3 -o "$f" "https://github.com/$t"; chmod +x "$f"
done
export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$qt/bin/qmake" LD_LIBRARY_PATH="$qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$tools:$PATH"

# the AppDir: the program, Qt (xcb only: Vulkan through X11 / XWayland), the licence and the notices beside it
rm -rf "$app"; mkdir -p "$app/usr/bin" "$app/usr/share/doc/hairharpy"
cp "$bld/HairHarpy" "$app/usr/bin/"
for f in LICENSE.md THIRD_PARTY_NOTICES.md TRUST.md TRUST.ru.md; do
  for p in "$root/public/$f" "$root/$f"; do [ -f "$p" ] && { cp "$p" "$app/usr/share/doc/hairharpy/"; break; }; done
done
linuxdeploy-x86_64.AppImage --appdir "$app" --executable "$app/usr/bin/HairHarpy" \
  --desktop-file "$here/hairharpy.desktop" --icon-file "$here/hairharpy.png" --plugin qt
# (no network plugins: the program's only connection is the bridge to 127.0.0.1 — TRUST.md)
rm -rf "$app/usr/plugins/tls" "$app/usr/plugins/networkinformation" "$app/usr/plugins/bearer"

mkdir -p "$dist"
appimage=HairHarpy-$ver-x86_64.AppImage; tarball=HairHarpy-$ver-linux-x86_64.tar.gz
rm -f "$dist/$appimage" "$dist/$tarball"
(cd "$dist" && LINUXDEPLOY_OUTPUT_VERSION=$ver OUTPUT=$appimage linuxdeploy-x86_64.AppImage --appdir "$app" --output appimage)

# the folder version: the AppDir as it is, with install.sh / uninstall.sh and the icon at its top
stage=$root/build/linux-stage; rm -rf "$stage"; mkdir -p "$stage"
cp -a "$app" "$stage/HairHarpy-$ver"
cp "$here/install.sh" "$here/uninstall.sh" "$here/hairharpy.png" "$stage/HairHarpy-$ver/"
chmod +x "$stage/HairHarpy-$ver/install.sh" "$stage/HairHarpy-$ver/uninstall.sh"
tar -C "$stage" -czf "$dist/$tarball" "HairHarpy-$ver"

(cd "$dist" && sha256sum "$appimage" "$tarball" > SHA256SUMS-linux.txt)
echo "Linux release: $dist/$appimage, $dist/$tarball, $dist/SHA256SUMS-linux.txt"
