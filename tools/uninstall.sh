#!/bin/sh
# Remove Barline: the program, its app-menu entries, and the kits. Your songs, set lists, beats,
# and settings stay (in ~/.local/share/barline) unless you add --everything.
set -e
S="${XDG_DATA_HOME:-$HOME/.local/share}"
D="$S/barline"
if [ -f "$D/barline.lock" ] && command -v flock >/dev/null 2>&1 && ! flock -n "$D/barline.lock" true 2>/dev/null; then
    echo "Barline is running: quit it first."
    exit 1
fi
rm -f "$HOME/.local/bin/barline" "$S/applications/barline.desktop" "$S/applications/barline-terminal.desktop" \
      "$S/icons/hicolor/scalable/apps/barline.svg"
rm -rf "$D/kits" "$D/window-profile"
if [ "$1" = "--everything" ]; then
    printf "Delete your songs, lyrics, set lists, beats, and settings too? Type yes: "
    read -r a
    if [ "$a" = yes ]; then
        rm -rf "$D"
        echo "Barline and everything of yours in it is gone."
        exit 0
    fi
fi
echo "Barline is removed. Your songs, set lists, beats, and settings are still in $D"
echo "(installing again picks them up; to delete them too: sh uninstall.sh --everything)."
