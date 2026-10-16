#include <cerrno>
#include "term.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace term {

namespace {

struct Cell {
    std::string ch = " ";
    short fg = -1, bg = -1;
    unsigned char attr = 0;
    bool operator==(const Cell &o) const { return ch == o.ch && fg == o.fg && bg == o.bg && attr == o.attr; }
};

int W = 80, H = 24;
std::vector<Cell> back, front;
bool active = false, use_ascii = false, full_redraw = true;
ColorMode color_mode = COLOR_256;
int cur_x = -1, cur_y = -1;

#ifdef _WIN32
HANDLE hin, hout;
DWORD old_in_mode = 0, old_out_mode = 0;
UINT old_cp = 0;
#else
termios old_tio;
volatile std::sig_atomic_t resized = 0;
volatile std::sig_atomic_t asked_to_quit = 0;   // SIGTERM / SIGHUP: quit the normal way (saved, the pedal told)
std::string inbuf;
#endif

void out(const std::string &s) {
#ifdef _WIN32
    DWORD n;
    WriteFile(hout, s.data(), (DWORD)s.size(), &n, nullptr);
#else
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
#endif
}

void query_size() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hout, &info)) {
        W = info.srWindow.Right - info.srWindow.Left + 1;
        H = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        W = ws.ws_col;
        H = ws.ws_row;
    }
#endif
    if (W < 1) W = 80;
    if (H < 1) H = 24;
    back.assign((size_t)W * H, Cell());
    front.assign((size_t)W * H, Cell());
    full_redraw = true;
}

// xterm-256 -> nearest of the 16 basic colors (for the Linux text console)
int to16(int c) {
    if (c < 16) return c;
    int r, g, b;
    if (c >= 232) {
        int v = 8 + (c - 232) * 10;
        r = g = b = v;
    } else {
        int i = c - 16;
        static const int lv[] = {0, 95, 135, 175, 215, 255};
        r = lv[i / 36];
        g = lv[(i / 6) % 6];
        b = lv[i % 6];
    }
    static const int pal[16][3] = {{0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0}, {0, 0, 238}, {205, 0, 205},
                                   {0, 205, 205}, {229, 229, 229}, {127, 127, 127}, {255, 0, 0}, {0, 255, 0},
                                   {255, 255, 0}, {92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    int best = 0, bd = 1 << 30;
    for (int k = 0; k < 16; k++) {
        int d = (r - pal[k][0]) * (r - pal[k][0]) + (g - pal[k][1]) * (g - pal[k][1]) + (b - pal[k][2]) * (b - pal[k][2]);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

std::string sgr(const Cell &c) {
    std::string s = "\x1b[0";
    if (c.attr & A_BOLD) s += ";1";
    if (c.attr & A_DIM) s += ";2";
    if (c.attr & A_UNDER) s += ";4";
    if (c.attr & A_REVERSE) s += ";7";
    if (color_mode == COLOR_256) {
        if (c.fg >= 0) s += ";38;5;" + std::to_string(c.fg);
        if (c.bg >= 0) s += ";48;5;" + std::to_string(c.bg);
    } else if (color_mode == COLOR_16) {
        if (c.fg >= 0) { int k = to16(c.fg); s += ";" + std::to_string(k < 8 ? 30 + k : 90 + k - 8); }
        if (c.bg >= 0) { int k = to16(c.bg); s += ";" + std::to_string(k < 8 ? 40 + k : 100 + k - 8); }
    }
    return s + "m";
}

#ifndef _WIN32
void on_winch(int) { resized = 1; }
void on_quit(int) { asked_to_quit = 1; }
void on_fatal(int sig) {
    stop();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

int utf8_decode(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + len > s.size()) { i = s.size(); return '?'; }
    int cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += len;
    return cp;
}

}  // namespace

bool start() {
#ifdef _WIN32
    hin = GetStdHandle(STD_INPUT_HANDLE);
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(hin, &old_in_mode) || !GetConsoleMode(hout, &old_out_mode)) return false;
    old_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(hout, old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    // mouse on, quick-edit off (it would swallow clicks), keep Ctrl-C as a key
    SetConsoleMode(hin, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    tcgetattr(STDIN_FILENO, &old_tio);
    termios t = old_tio;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    std::signal(SIGWINCH, on_winch);
    std::signal(SIGTERM, on_quit);
    std::signal(SIGHUP, on_quit);
    std::signal(SIGSEGV, on_fatal);
    std::signal(SIGABRT, on_fatal);
#endif
    active = true;
    std::atexit(stop);
    // alternate screen, hide cursor, mouse clicks + wheel (SGR coordinates)
    out("\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[2J");
    query_size();
    return true;
}

void stop() {
    if (!active) return;
    active = false;
    out("\x1b[0m\x1b[?1000l\x1b[?1006l\x1b[?25h\x1b[?1049l");
#ifdef _WIN32
    SetConsoleMode(hin, old_in_mode);
    SetConsoleMode(hout, old_out_mode);
    if (old_cp) SetConsoleOutputCP(old_cp);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_tio);
#endif
}

void set_color_mode(ColorMode m) { color_mode = m; full_redraw = true; }
void set_ascii(bool on) { use_ascii = on; full_redraw = true; }
bool ascii() { return use_ascii; }
int width() { return W; }
int height() { return H; }

// --- input -------------------------------------------------------------------

#ifdef _WIN32
Event read(int timeout_ms) {
    Event e;
    DWORD w = WaitForSingleObject(hin, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (w == WAIT_FAILED) { e.key = K_CLOSED; return e; }   // the console went away
    if (w != WAIT_OBJECT_0) return e;
    INPUT_RECORD rec;
    DWORD n = 0;
    if (!ReadConsoleInputW(hin, &rec, 1, &n)) { e.key = K_CLOSED; return e; }
    if (n == 0) return e;
    if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        query_size();
        e.key = K_RESIZE;
        return e;
    }
    if (rec.EventType == MOUSE_EVENT) {
        const MOUSE_EVENT_RECORD &m = rec.Event.MouseEvent;
        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(hout, &info);
        e.mx = m.dwMousePosition.X - info.srWindow.Left;
        e.my = m.dwMousePosition.Y - info.srWindow.Top;
        if (m.dwEventFlags == MOUSE_WHEELED) {
            e.key = K_MOUSE;
            e.button = ((short)HIWORD(m.dwButtonState) > 0) ? 64 : 65;
        } else if (m.dwEventFlags == 0 && m.dwButtonState) {
            e.key = K_MOUSE;
            e.button = (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) ? 0 : (m.dwButtonState & RIGHTMOST_BUTTON_PRESSED) ? 2 : 1;
        }
        return e;
    }
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) return e;
    const KEY_EVENT_RECORD &k = rec.Event.KeyEvent;
    bool ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    bool shift = (k.dwControlKeyState & SHIFT_PRESSED) != 0;
    switch (k.wVirtualKeyCode) {
        case VK_UP: e.key = K_UP; return e;
        case VK_DOWN: e.key = K_DOWN; return e;
        case VK_LEFT: e.key = K_LEFT; return e;
        case VK_RIGHT: e.key = K_RIGHT; return e;
        case VK_HOME: e.key = K_HOME; return e;
        case VK_END: e.key = K_END; return e;
        case VK_PRIOR: e.key = K_PGUP; return e;
        case VK_NEXT: e.key = K_PGDN; return e;
        case VK_DELETE: e.key = K_DELETE; return e;
        case VK_INSERT: e.key = K_INSERT; return e;
        case VK_F1: e.key = K_F1; return e;
        case VK_RETURN: e.key = K_ENTER; return e;
        case VK_ESCAPE: e.key = K_ESC; return e;
        case VK_BACK: e.key = K_BACKSPACE; return e;
        case VK_TAB: e.key = shift ? K_BACKTAB : K_TAB; return e;
    }
    wchar_t ch = k.uChar.UnicodeChar;
    if (!ch) return e;
    if (ctrl && ch >= 1 && ch <= 26) {
        e.key = 'a' + ch - 1;
        e.ctrl = true;
        return e;
    }
    static wchar_t high = 0;
    if (ch >= 0xD800 && ch <= 0xDBFF) { high = ch; return e; }
    if (ch >= 0xDC00 && ch <= 0xDFFF && high) {
        e.key = 0x10000 + ((high - 0xD800) << 10) + (ch - 0xDC00);
        high = 0;
        return e;
    }
    e.key = ch;
    return e;
}
#else
static bool input_closed = false;

static bool fill_input(int timeout_ms) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r < 0 && errno != EINTR) { input_closed = true; return false; }
    if (r <= 0) return false;
    if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(p.revents & POLLIN)) { input_closed = true; return false; }
    char buf[256];
    ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) { input_closed = true; return false; }
    if (n < 0) return false;
    inbuf.append(buf, (size_t)n);
    return true;
}

static Event parse_escape() {
    // inbuf[0] == ESC. Returns K_NONE in e.key when the sequence is incomplete.
    Event e;
    if (inbuf.size() == 1) {
        e.key = K_ESC;
        inbuf.erase(0, 1);
        return e;
    }
    char c1 = inbuf[1];
    if (c1 == 'O' && inbuf.size() >= 3) {
        char c = inbuf[2];
        inbuf.erase(0, 3);
        switch (c) {
            case 'A': e.key = K_UP; break;
            case 'B': e.key = K_DOWN; break;
            case 'C': e.key = K_RIGHT; break;
            case 'D': e.key = K_LEFT; break;
            case 'H': e.key = K_HOME; break;
            case 'F': e.key = K_END; break;
            case 'P': e.key = K_F1; break;
            default: e.key = K_NONE;
        }
        return e;
    }
    if (c1 != '[') {  // Alt+key: treat as ESC then the key
        e.key = K_ESC;
        inbuf.erase(0, 1);
        return e;
    }
    // CSI: ESC [ params final
    size_t i = 2;
    while (i < inbuf.size() && !((inbuf[i] >= 0x40 && inbuf[i] <= 0x7E) && inbuf[i] != '[' && inbuf[i] != '<')) i++;
    if (i >= inbuf.size()) {
        e.key = -1;  // incomplete
        return e;
    }
    std::string params = inbuf.substr(2, i - 2);
    char fin = inbuf[i];
    inbuf.erase(0, i + 1);
    if (!params.empty() && params[0] == '<') {  // SGR mouse: <b;x;yM
        int b = 0, x = 0, y = 0;
        std::sscanf(params.c_str() + 1, "%d;%d;%d", &b, &x, &y);
        if (fin == 'm') return e;  // release
        if (b & 32) return e;      // motion
        e.key = K_MOUSE;
        e.button = (b & 64) ? (64 + (b & 1)) : (b & 3);
        e.mx = x - 1;
        e.my = y - 1;
        return e;
    }
    int n1 = std::atoi(params.c_str());
    bool ctrl = params.find(";5") != std::string::npos;
    e.ctrl = ctrl;
    switch (fin) {
        case 'A': e.key = K_UP; break;
        case 'B': e.key = K_DOWN; break;
        case 'C': e.key = K_RIGHT; break;
        case 'D': e.key = K_LEFT; break;
        case 'H': e.key = K_HOME; break;
        case 'F': e.key = K_END; break;
        case 'Z': e.key = K_BACKTAB; break;
        case '~':
            switch (n1) {
                case 1: case 7: e.key = K_HOME; break;
                case 2: e.key = K_INSERT; break;
                case 3: e.key = K_DELETE; break;
                case 4: case 8: e.key = K_END; break;
                case 5: e.key = K_PGUP; break;
                case 6: e.key = K_PGDN; break;
                case 11: e.key = K_F1; break;
            }
            break;
    }
    return e;
}

Event read(int timeout_ms) {
    Event e;
    for (;;) {
        if (resized) {
            resized = 0;
            query_size();
            e.key = K_RESIZE;
            return e;
        }
        if (input_closed || asked_to_quit) { e.key = K_CLOSED; return e; }
        if (inbuf.empty() && !fill_input(timeout_ms)) { if (input_closed || asked_to_quit) e.key = K_CLOSED; return e; }
        if (inbuf.empty()) continue;
        unsigned char c = (unsigned char)inbuf[0];
        if (c == 0x1b) {
            if (inbuf.size() == 1) fill_input(25);  // lone ESC or the start of a sequence?
            Event k = parse_escape();
            if (k.key == -1) {  // wait a little for the rest
                if (!fill_input(50)) { inbuf.clear(); return e; }
                continue;
            }
            return k;
        }
        if (c == '\r' || c == '\n') { inbuf.erase(0, 1); e.key = K_ENTER; return e; }
        if (c == 0x7f || c == 0x08) { inbuf.erase(0, 1); e.key = K_BACKSPACE; return e; }
        if (c == '\t') { inbuf.erase(0, 1); e.key = K_TAB; return e; }
        if (c < 32) {
            inbuf.erase(0, 1);
            e.key = 'a' + c - 1;
            e.ctrl = true;
            return e;
        }
        size_t i = 0;
        if (c >= 0x80) {
            unsigned need = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            if (inbuf.size() < need) fill_input(20);
        }
        e.key = utf8_decode(inbuf, i);
        inbuf.erase(0, i);
        return e;
    }
}
#endif

// --- drawing -----------------------------------------------------------------

void clear(int bg) {
    for (auto &c : back) { c = Cell(); c.bg = (short)bg; }
    cur_x = cur_y = -1;
}

// One character as it may safely reach the terminal. Names can come from outside (a server,
// an imported file), and a control character in one could move the cursor, change colors, or
// worse, so control characters (C0, DEL, C1) and broken UTF-8 show as "?".
static std::string printable(const std::string &text, size_t start, size_t end, int cp) {
    unsigned char lead = (unsigned char)text[start];
    bool broken = end - start == 1 && lead >= 0x80;
    for (size_t k = start + 1; k < end; k++)
        if (((unsigned char)text[k] & 0xC0) != 0x80) broken = true;
    if (cp == '\t' || cp == '\n' || cp == '\r') return " ";
    if (broken || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) return "?";
    return text.substr(start, end - start);
}

void put(int x, int y, const std::string &text, int fg, int bg, int attr) {
    if (y < 0 || y >= H) return;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = i;
        int cp = utf8_decode(text, i);
        if (x >= W) break;
        if (x >= 0) {
            Cell &c = back[(size_t)y * W + x];
            c.ch = printable(text, start, i, cp);
            c.fg = (short)fg;
            if (bg != -2) c.bg = (short)bg;
            c.attr = (unsigned char)attr;
        }
        x++;
    }
}

void fill(int x, int y, int w, int h, int bg, const std::string &ch, int fg) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < W && yy >= 0 && yy < H) {
                Cell &c = back[(size_t)yy * W + xx];
                c.ch = ch;
                c.fg = (short)fg;
                c.bg = (short)bg;
                c.attr = 0;
            }
}

void cursor(int x, int y) { cur_x = x; cur_y = y; }

void invalidate() { full_redraw = true; }

void present() {
    std::string s = "\x1b[?25l";
    if (full_redraw) s += "\x1b[0m\x1b[2J";
    std::string last_sgr;
    int px = -1, py = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t k = (size_t)y * W + x;
            if (!full_redraw && back[k] == front[k]) continue;
            if (y == H - 1 && x == W - 1) continue;  // writing the last cell can scroll some terminals
            if (px != x || py != y) s += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
            std::string g = sgr(back[k]);
            if (g != last_sgr) { s += g; last_sgr = g; }
            s += back[k].ch;
            px = x + 1;
            py = y;
            front[k] = back[k];
        }
    }
    s += "\x1b[0m";
    if (cur_x >= 0 && cur_y >= 0) s += "\x1b[" + std::to_string(cur_y + 1) + ";" + std::to_string(cur_x + 1) + "H\x1b[?25h";
    full_redraw = false;
    out(s);
}

// --- glyphs ------------------------------------------------------------------

std::string g_block() { return use_ascii ? "#" : "\xE2\x96\x88"; }
std::string g_shade() { return use_ascii ? "." : "\xE2\x96\x91"; }
std::string g_hline() { return use_ascii ? "-" : "\xE2\x94\x80"; }
std::string g_vline() { return use_ascii ? "|" : "\xE2\x94\x82"; }
std::string g_arrow_up() { return use_ascii ? "^" : "\xE2\x96\xB2"; }
std::string g_arrow_down() { return use_ascii ? "v" : "\xE2\x96\xBC"; }
std::string g_bullet() { return use_ascii ? "*" : "\xE2\x80\xA2"; }

std::string g_spark(int level) {
    if (level < 0) level = 0;
    if (level > 7) level = 7;
    if (use_ascii) return std::string(1, "_.-=+*#@"[level]);
    static const char *s[] = {"\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
                              "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};
    return s[level];
}

std::string g_partial(int e) {
    if (e <= 0) return " ";
    if (use_ascii) return e >= 4 ? "#" : " ";
    static const char *s[] = {"", "\xE2\x96\x8F", "\xE2\x96\x8E", "\xE2\x96\x8D", "\xE2\x96\x8C",
                              "\xE2\x96\x8B", "\xE2\x96\x8A", "\xE2\x96\x89"};
    return e >= 8 ? g_block() : s[e];
}

}  // namespace term
