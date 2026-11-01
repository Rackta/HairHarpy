#!/bin/sh
# HairHarpy for Linux — remove what install.sh made: the program folder (this one), the menu entry, the icon, the
# .hairmesh association. Your projects, presets, autosaves and settings stay (~/.local/share/Catarackta/HairHarpy,
# ~/.config/Catarackta/HairHarpy.conf) — delete them yourself if you want.
set -eu
dir=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}
[ -x "$dir/usr/bin/HairHarpy" ] || { echo "This is not a HairHarpy folder: $dir"; exit 1; }
rm -f "$data/applications/hairharpy.desktop" "$data/icons/hicolor/256x256/apps/hairharpy.png" "$data/mime/packages/hairharpy.xml"
command -v update-mime-database >/dev/null && update-mime-database "$data/mime" >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null && update-desktop-database "$data/applications" >/dev/null 2>&1 || true
rm -rf "$dir"
echo "HairHarpy removed. Your projects and settings are kept in $data/Catarackta/HairHarpy and ~/.config/Catarackta."
