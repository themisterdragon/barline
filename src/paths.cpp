#include "paths.h"

#include <cstdio>
#include <cstdlib>

#include "sys.h"

namespace paths {

// Linux: ~/.local/share/barline. macOS: ~/Library/Application Support/Barline. Windows: %APPDATA%/Barline.
std::string data() {
#if defined(_WIN32)
    const char *a = std::getenv("APPDATA");
    std::string base = a && *a ? a : std::string(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".");
    for (auto &c : base)
        if (c == '\\') c = '/';
    std::string d = base + "/Barline";
#elif defined(__APPLE__)
    const char *h = std::getenv("HOME");
    std::string base = std::string(h ? h : ".") + "/Library/Application Support";
    std::string d = base + "/Barline";
#else
    const char *h = std::getenv("HOME");
    const char *x = std::getenv("XDG_DATA_HOME");
    std::string base = x && *x ? x : std::string(h ? h : ".") + "/.local/share";
    std::string d = base + "/barline";
#endif
    sys::make_dir(base, 0755);
    sys::make_dir(d, 0700);
    return d;
}

std::string kits() {
    if (const char *k = std::getenv("BARLINE_KITS"))
        if (*k) return k;
    std::string self = sys::exe_path();
    if (!self.empty()) {
        std::string dir = self.substr(0, self.rfind('/'));
        if (sys::is_dir(dir + "/kits")) return dir + "/kits";
        if (sys::is_dir(dir + "/../Resources/kits")) return dir + "/../Resources/kits";   // inside Barline.app
        if (sys::is_dir(dir + "/../share/barline/kits")) return dir + "/../share/barline/kits";
    }
    return data() + "/kits";
}

}  // namespace paths
