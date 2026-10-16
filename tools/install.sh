#!/bin/sh
# Install Barline for this user (no root needed): the program in ~/.local/bin, the kits in
# ~/.local/share/barline, and Barline in the app menu. Your settings, songs, and beats are kept.
# An update without a kits folder keeps the kits you have.
set -e
cd "$(dirname "$0")"
D="${XDG_DATA_HOME:-$HOME/.local/share}/barline"
BIN="$HOME/.local/bin/barline"

# the program for this computer (Intel/AMD, or ARM like a Raspberry Pi)
case "$(uname -m)" in
    x86_64 | amd64) P=barline-x86_64 ;;
    aarch64 | arm64) P=barline-aarch64 ;;
    *) P="" ;;
esac
[ -n "$P" ] && [ -f "$P" ] || P=barline   # (an older package: one program)
if [ ! -f "$P" ]; then
    echo "This package has no Barline for a $(uname -m) computer."
    exit 1
fi
if [ "$(uname -s)" != Linux ]; then
    echo "Barline runs on Linux (it talks to the sound and MIDI devices directly)."
    exit 1
fi
if [ -f "$D/barline.lock" ] && command -v flock >/dev/null 2>&1 && ! flock -n "$D/barline.lock" true 2>/dev/null; then
    echo "Barline is running: quit it first (q, or Settings > Quit in its window), then install again."
    exit 1
fi


mkdir -p "$HOME/.local/bin" "$D"
install -m 755 "$P" "$BIN"
if [ -d kits ]; then
    rm -rf "$D/kits.new"
    cp -r kits "$D/kits.new"
    rm -rf "$D/kits"
    mv "$D/kits.new" "$D/kits"
fi
# single kits an update adds (the others stay as they are)
if [ -d kits-add ]; then
    mkdir -p "$D/kits"
    for k in kits-add/*/; do
        k=$(basename "$k")
        rm -rf "$D/kits/$k"
        cp -r "kits-add/$k" "$D/kits/$k"
        echo "Added the $k kit."
    done
fi
[ -f KITS.md ] && cp KITS.md "$D/"
[ -f GUIDE.txt ] && cp GUIDE.txt "$D/"
# beats this update brings: added to My Beats (a beat of yours with the same file name is kept)
if [ -d beats ]; then
    mkdir -p "$D/beats"
    for b in beats/*.beat; do
        [ -e "$b" ] || continue
        [ -e "$D/beats/$(basename "$b")" ] || cp "$b" "$D/beats/"
    done
fi
# song settings this update brings: Barline fills them in the next time it starts (once)
if [ -d pending ]; then
    mkdir -p "$D/pending"
    for f in pending/*.csv pending/beat-ids*.txt; do [ -e "$f" ] && cp "$f" "$D/pending/"; done
    echo "Song changes this update brings are applied the next time Barline starts."
fi

# the app menu: "Barline" (its window) and "Barline (terminal)"
if [ -d share ]; then
    A="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    I="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
    mkdir -p "$A" "$I"
    cp share/barline.svg "$I/barline.svg"
    for f in barline.desktop barline-terminal.desktop; do
        sed "s|@BIN@|$BIN|g" "share/$f" > "$A/$f"
    done
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$A" >/dev/null 2>&1 || true
fi

echo "Installed Barline $("$BIN" --version 2>/dev/null | cut -d' ' -f2)."
case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *) echo "Note: $HOME/.local/bin isn't on your PATH, so type $BIN (or add it to your PATH). The app menu works either way." ;;
esac
echo "Start it from the app menu (Barline), or type: barline        Check the setup any time: barline doctor"
