# Barline

Drums, a click, and perfectly timed loops for a USB looper pedal, from a laptop: Linux, Windows,
or macOS.

- Drum beats in many styles on real, openly licensed drum kits (and a few synthesized ones),
  with a beat builder for your own.
- MIDI clock to the pedal, and pedal presses sent exactly on the bar line, so loops come out
  the right length every time ("record 4 bars" and it's done). One press records a verse loop
  and a chorus loop back to back.
- Play it from the laptop's keyboard or a footswitch, its own window, or an iPad/phone on the
  same Wi-Fi (or your own router plugged into the laptop): lyrics, set lists, and the
  transport always in reach.
- One program, nothing else to install: Linux (Intel/AMD or ARM, static, straight to the sound
  driver), Windows 10/11 (WASAPI and Windows MIDI; Intel/AMD or ARM), and macOS 11+ (Core Audio and
  Core MIDI; Apple silicon and Intel).

How to use it: GUIDE.txt. What changed: CHANGES.txt.

## Building

`sh tools/build.sh` (quick, this computer), `sh tools/build.sh release` (static Linux x86_64 +
aarch64 with Zig), `sh tools/build.sh windows` (Windows, also with Zig), `sh tools/build.sh mac`
(on a Mac), and `sh tools/package.sh` (the packages, with the kits; see the script). GitHub builds
and tries all three on every push (.github/workflows/build.yml). Kits are
built from their open-source sample libraries with `tools/kits/build.py` (see
`tools/kits/kits.py` for each kit's source and license).

## How it talks to the pedal

Only through what the pedal's published user guide documents: MIDI notes and clock over USB,
and the pedal's USB audio input. Nothing from the pedal's firmware or internals is read,
copied, or changed.

## License and credits

Barline is free software under the GNU General Public License, version 3 (LICENSE).
The drum kits are made from samples under CC0-1.0 and CC-BY-4.0; their credits are in KITS.md.

Barline is an independent project, not affiliated with or endorsed by any pedal or app maker.
Product names, where they come up, belong to their owners.