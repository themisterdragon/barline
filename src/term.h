// Terminal layer: raw keyboard/mouse input and a diffed screen buffer.
// Plain escape codes on Linux/macOS; the console API on Windows. No curses needed.
#pragma once
#include <string>
#include <vector>

namespace term {

enum Key {
    K_NONE = 0,
    K_ENTER = 0x110000, K_ESC, K_BACKSPACE, K_TAB, K_BACKTAB, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_DELETE, K_INSERT, K_F1,
    K_MOUSE, K_RESIZE,
    K_CLOSED,                 // the terminal went away (window closed, SSH dropped): save and quit
};

struct Event {
    int key = K_NONE;         // a Key, or a Unicode code point for typed characters
    bool ctrl = false;
    int mx = 0, my = 0;       // mouse cell for K_MOUSE
    int button = 0;           // 0 left, 1 middle, 2 right, 64 wheel up, 65 wheel down
};

// Colors are xterm-256 indexes; -1 = terminal default.
enum Attr { A_BOLD = 1, A_UNDER = 2, A_REVERSE = 4, A_DIM = 8 };

enum ColorMode { COLOR_256 = 0, COLOR_16 = 1, COLOR_NONE = 2 };

bool start();          // enter full-screen mode; false if stdin/stdout isn't a terminal
void stop();           // always restore the terminal (also called at exit / on signals)
void set_color_mode(ColorMode m);
void set_ascii(bool on);   // draw with plain ASCII instead of Unicode blocks/lines
bool ascii();

int width();
int height();

Event read(int timeout_ms);   // K_NONE on timeout

// Drawing into the back buffer
void clear(int bg = -1);
void put(int x, int y, const std::string &text, int fg = -1, int bg = -1, int attr = 0);
void fill(int x, int y, int w, int h, int bg, const std::string &ch = " ", int fg = -1);
void cursor(int x, int y);    // show the text cursor here this frame (-1 hides)
void present();               // send only what changed since the last frame
void invalidate();            // repaint everything next present()

// Glyphs that fall back to ASCII
std::string g_block();        // █ or #
std::string g_shade();        // ░ or .
std::string g_hline();        // ─ or -
std::string g_vline();        // │ or |
std::string g_spark(int level);   // 0..7 → ▁..█ or .:-=+*#@
std::string g_partial(int eighths);  // 1..7 eighths of a cell for smooth bars
std::string g_arrow_up();
std::string g_arrow_down();
std::string g_bullet();

}  // namespace term
