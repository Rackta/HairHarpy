#!/bin/sh
# HairHarpy for Linux — install for your user (no root): the program folder, a menu entry, .hairmesh opened with it.
#   sh install.sh [--dir <folder>] [--no-menu] [--no-assoc]
# Default folder: ~/.local/share/HairHarpy. Running it again over an installation updates the program. Your projects,
# presets, autosaves and settings are not in that folder (~/.local/share/Catarackta/HairHarpy, ~/.config/Catarackta),
# so an update or uninstall.sh never touches them.
set -eu
src=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
dir=$data/HairHarpy; menu=1; assoc=1
while [ $# -gt 0 ]; do case $1 in
  --dir) dir=$2; shift 2 ;; --no-menu) menu=0; shift ;; --no-assoc) assoc=0; shift ;;
  *) echo "unknown option: $1"; exit 2 ;; esac; done
[ -x "$src/usr/bin/HairHarpy" ] || { echo "Run install.sh from the unpacked HairHarpy folder"; exit 1; }

if [ "$src" != "$dir" ]; then
  if [ -e "$dir" ] && [ ! -x "$dir/usr/bin/HairHarpy" ]; then echo "$dir exists and is not a HairHarpy folder: pick another with --dir"; exit 1; fi
  rm -rf "$dir"; mkdir -p "$(dirname "$dir")"; cp -a "$src" "$dir"
fi

if [ $menu = 1 ]; then
  mkdir -p "$data/applications" "$data/icons/hicolor/256x256/apps"
  cp "$dir/hairharpy.png" "$data/icons/hicolor/256x256/apps/hairharpy.png"
  sed -e "s|^Exec=.*|Exec=\"$dir/AppRun\" %f|" "$dir/hairharpy.desktop" > "$data/applications/hairharpy.desktop"
  [ $assoc = 1 ] || sed -i '/^MimeType=/d' "$data/applications/hairharpy.desktop"
fi
if [ $assoc = 1 ]; then
  mkdir -p "$data/mime/packages"
  cat > "$data/mime/packages/hairharpy.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-hairmesh">
    <comment>HairHarpy project</comment>
    <glob pattern="*.hairmesh"/>
  </mime-type>
</mime-info>
EOF
  command -v update-mime-database >/dev/null && update-mime-database "$data/mime" >/dev/null 2>&1 || true
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$data/icons/hicolor" >/dev/null 2>&1 || true

echo "HairHarpy installed into $dir"
[ $menu = 1 ] && echo "Start it from the application menu, or: \"$dir/AppRun\""
echo "Uninstall: sh \"$dir/uninstall.sh\""
