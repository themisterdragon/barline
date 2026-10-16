// Where Barline keeps things: its kits, and your settings, songs, and set lists.
#pragma once
#include <string>

namespace paths {
std::string data();   // where your things live (made if missing): ~/.local/share/barline on Linux
std::string kits();   // BARLINE_KITS, or kits/ beside the program, or <data>/kits
}
