#!/bin/sh
# Build Barline.
#   sh tools/build.sh            a quick build for this computer (build/barline, with the system's c++)
#   sh tools/build.sh release    static programs that run on any Linux: build/barline-x86_64, -aarch64
#   sh tools/build.sh windows    Windows programs (Intel/AMD and ARM), made here with Zig
#   sh tools/build.sh mac        the macOS program (on a Mac: Apple's clang, Apple silicon + Intel in one)
#   sh tools/build.sh asan       a checked build for testing (build/barline-asan); tsan: threads checked
# Release builds need Zig (ZIG=/path/to/zig, or zig on the PATH); nothing else.
set -e
cd "$(dirname "$0")/.."
python3 tools/embed_web.py >/dev/null
mkdir -p build
case "${1:-dev}" in
dev)
    ${CXX:-c++} -std=c++17 -O2 -Wall -Wextra -pthread -o build/barline src/*.cpp
    echo "build/barline $(./build/barline --version)"
    ;;
asan)
    ${CXX:-c++} -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -o build/barline-asan src/*.cpp
    echo "build/barline-asan"
    ;;
tsan)
    ${CXX:-c++} -std=c++17 -O1 -g -fsanitize=thread -pthread -o build/barline-tsan src/*.cpp
    echo "build/barline-tsan"
    ;;
release)
    Z="${ZIG:-$(command -v zig || echo "$HOME/.cache/tally-build/zig/zig")}"
    [ -x "$Z" ] || { echo "Release builds need Zig: set ZIG=/path/to/zig"; exit 1; }
    for arch in x86_64 aarch64; do
        "$Z" c++ -target $arch-linux-musl -std=c++17 -O2 -DNDEBUG -s -static -pthread -o build/barline-$arch src/*.cpp
        echo "build/barline-$arch"
    done
    ;;
windows)
    # barline-windows-<arch>.exe (shipped as barline.exe): double-clicked, it opens the window; typed in a
    # terminal, the terminal screen and every command. Made here with Zig, for Intel/AMD and ARM PCs.
    Z="${ZIG:-$(command -v zig || echo "$HOME/.cache/tally-build/zig/zig")}"
    [ -x "$Z" ] || { echo "Windows builds need Zig: set ZIG=/path/to/zig"; exit 1; }
    for arch in x86_64 aarch64; do
        (cd tools/win && "$Z" c++ -target $arch-windows-gnu -std=c++17 -O2 -DNDEBUG -s -o "../../build/barline-windows-$arch.exe" \
            ../../src/*.cpp barline.rc -lole32 -lwinmm -lws2_32 -liphlpapi -lshell32)
        echo "build/barline-windows-$arch.exe"
    done
    ;;
mac)
    # on a Mac (Apple's compilers and its Core Audio / Core MIDI): one program for Apple silicon and Intel
    ${CXX:-clang++} -std=c++17 -O2 -DNDEBUG -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -o build/barline-mac src/*.cpp \
        -framework CoreAudio -framework AudioToolbox -framework CoreMIDI -framework CoreFoundation
    echo "build/barline-mac"
    ;;
*)
    echo "usage: sh tools/build.sh [dev|release|windows|mac|asan|tsan]"
    exit 2
    ;;
esac
