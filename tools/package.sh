#!/bin/sh
# Make the Barline packages, each with the kits, the guide, and the license:
#   sh tools/package.sh                   Linux: build/dist/Barline-<v>-linux.tar.gz (Intel/AMD + ARM, installer)
#   sh tools/package.sh --no-kits         Linux, no kits: an update for an installed copy
#   sh tools/package.sh --windows         Windows: Barline-<v>-windows-x64.zip and -arm64.zip (unzip, double-click)
#   sh tools/package.sh --mac BARLINE     macOS: Barline-<v>-mac.zip with Barline.app, from a program
#                                          built on a Mac (sh tools/build.sh mac, or the GitHub build)
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/.*BARLINE_VERSION "\(.*\)"/\1/p' src/version.h)
mkdir -p build/dist
need_kits() { [ -d build/kits ] || { echo "No kits in build/kits: build them with tools/kits/build.py first."; exit 1; }; }
done_msg() { echo "build/dist/$1 ($(du -h "build/dist/$1" | cut -f1))"; }

case "$1" in
--windows)
    need_kits
    sh tools/build.sh windows
    for arch in x86_64 aarch64; do
        tag=$([ $arch = x86_64 ] && echo x64 || echo arm64)
        NAME="Barline-$V-windows-$tag"
        OUT="build/dist/$NAME"
        rm -rf "$OUT" "build/dist/$NAME.zip"
        mkdir -p "$OUT"
        cp "build/barline-windows-$arch.exe" "$OUT/barline.exe"
        cp GUIDE.txt KITS.md LICENSE tools/win/README-Windows.txt "$OUT/"
        cp -r build/kits "$OUT/kits"
        (cd build/dist && zip -qr "$NAME.zip" "$NAME")
        done_msg "$NAME.zip"
    done
    ;;
--mac)
    BIN="$2"
    [ -f "$BIN" ] || { echo "usage: sh tools/package.sh --mac path/to/barline-mac (built on a Mac)"; exit 2; }
    need_kits
    NAME="Barline-$V-mac"
    OUT="build/dist/$NAME"
    APP="$OUT/Barline.app/Contents"
    rm -rf "$OUT" "build/dist/$NAME.zip"
    mkdir -p "$APP/MacOS" "$APP/Resources"
    cp "$BIN" "$APP/MacOS/barline"
    chmod 755 "$APP/MacOS/barline"
    sed "s/@VERSION@/$V/g" tools/mac/Info.plist > "$APP/Info.plist"
    cp tools/mac/barline.icns "$APP/Resources/barline.icns"
    cp -r build/kits "$APP/Resources/kits"
    cp GUIDE.txt KITS.md LICENSE tools/mac/README-Mac.txt "$OUT/"
    (cd build/dist && zip -qry "$NAME.zip" "$NAME")
    done_msg "$NAME.zip"
    ;;
*)
    sh tools/build.sh release
    NAME="Barline-$V-linux"
    [ "$1" = "--no-kits" ] && NAME="Barline-$V-update"
    OUT="build/dist/$NAME"
    rm -rf "$OUT"
    mkdir -p "$OUT/share"
    cp build/barline-x86_64 build/barline-aarch64 tools/install.sh tools/uninstall.sh GUIDE.txt KITS.md LICENSE "$OUT/"
    cp tools/share/* "$OUT/share/"
    if [ "$1" != "--no-kits" ]; then
        need_kits
        cp -r build/kits "$OUT/kits"
    fi
    tar czf "build/dist/$NAME.tar.gz" -C build/dist "$NAME"
    done_msg "$NAME.tar.gz"
    ;;
esac
