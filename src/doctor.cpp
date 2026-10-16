#include "doctor.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <dirent.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app.h"
#include "config.h"
#include "http.h"
#include "kit.h"
#include "net.h"
#include "paths.h"
#include "snd.h"
#include "sys.h"
#include "version.h"

namespace doctor {

namespace {

int problems = 0;

void good(const std::string &s) { std::printf("  ok    %s\n", s.c_str()); }
void tip(const std::string &s) { std::printf("        %s\n", s.c_str()); }
void bad(const std::string &s) {
    std::printf("  !!    %s\n", s.c_str());
    problems++;
}
void maybe(const std::string &s) { std::printf("  --    %s\n", s.c_str()); }
void head(const std::string &s) { std::printf("\n%s\n", s.c_str()); }

bool exists(const std::string &p) { return sys::exists(p); }

#ifndef _WIN32

// a program with this name is running (PipeWire, PulseAudio...)
bool running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (dirent *e = readdir(d)) {
        if (!std::isdigit((unsigned char)e->d_name[0])) continue;
        std::ifstream f(std::string("/proc/") + e->d_name + "/comm");
        std::string comm;
        if (std::getline(f, comm) && comm == name) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

std::string in_path(const char *name) {
    const char *p = std::getenv("PATH");
    std::string path = p ? p : "/usr/bin:/bin";
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string full = path.substr(start, end - start) + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return full;
        start = end + 1;
    }
    return "";
}

bool port_free(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    bool ok = bind(s, (sockaddr *)&a, sizeof a) == 0;
    close(s);
    return ok;
}

// PipeWire's session manager, told to leave the pedal alone (so it never grabs the pedal's sound)
const char *WP_CONF =
    "# Barline: PipeWire leaves the looper pedal alone, so Barline can always use it.\n"
    "# Delete this file (and restart WirePlumber, or log out and in) to undo.\n"
    "monitor.alsa.rules = [\n"
    "  {\n"
    "    matches = [\n"
    "      { device.name = \"~.*[Ll]ooper.*\" }\n"
    "      { device.product.name = \"~.*[Ll]ooper.*\" }\n"
    "      { device.description = \"~.*[Ll]ooper.*\" }\n"
    "    ]\n"
    "    actions = { update-props = { device.disabled = true } }\n"
    "  }\n"
    "]\n";
const char *WP_LUA =
    "-- Barline: PipeWire leaves the looper pedal alone (WirePlumber 0.4). Delete this file to undo.\n"
    "table.insert(alsa_monitor.rules, {\n"
    "  matches = { { { \"device.name\", \"matches\", \"*[Ll]ooper*\" } }, { { \"device.product.name\", \"matches\", \"*[Ll]ooper*\" } } },\n"
    "  apply_properties = { [\"device.disabled\"] = true },\n"
    "})\n";

std::string config_home() {
    const char *x = std::getenv("XDG_CONFIG_HOME");
    const char *h = std::getenv("HOME");
    return x && *x ? x : std::string(h ? h : ".") + "/.config";
}

bool write_file(const std::string &dir, const std::string &name, const char *text) {
    std::string path;
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') sys::make_dir(dir.substr(0, i), 0755);
    path = dir + "/" + name;
    std::ofstream f(path, std::ios::trunc);
    f << text;
    return (bool)f;
}
#else
bool running(const char *) { return false; }
std::string in_path(const char *) { return ""; }
#endif

}  // namespace

int run(bool fix) {
    std::printf("Barline %s check-up\n", BARLINE_VERSION);
    std::string data = paths::data();
    bool sound_server = running("pipewire") || running("pulseaudio");
    bool wireplumber = running("wireplumber");

    if (fix) {
#ifndef __linux__
        std::printf("\nNothing to fix here: on this system no sound server takes the pedal away from Barline.\n");
        return 0;
#else
        if (!wireplumber) {
            std::printf("\nNothing to fix: PipeWire's WirePlumber isn't running here, so nothing else grabs the pedal.\n");
            return 0;
        }
        std::string d5 = config_home() + "/wireplumber/wireplumber.conf.d", d4 = config_home() + "/wireplumber/main.lua.d";
        bool ok = write_file(d5, "51-barline-looper.conf", WP_CONF) && write_file(d4, "51-barline-looper.lua", WP_LUA);
        if (!ok) {
            std::printf("\nCouldn't write the PipeWire setting in %s/wireplumber.\n", config_home().c_str());
            return 1;
        }
        std::printf("\nTold PipeWire to leave the pedal alone:\n  %s/51-barline-looper.conf\n  %s/51-barline-looper.lua\n"
                    "Now restart it (or log out and back in):  systemctl --user restart wireplumber\n"
                    "To undo: delete those two files and restart it again.\n",
                    d5.c_str(), d4.c_str());
        return 0;
#endif
    }

    // ---- is Barline running? (then it has the devices, and they can't be tried here)
    bool in_use = sys::lock_is_held(data + "/barline.lock");

    head("Barline");
    good("Your settings, songs, and beats: " + data);
    if (in_use) maybe("Barline is running now, so the pedal can't be tried from here. Quit it for the full check.");
    auto kits = kit::list(paths::kits());
    if (kits.empty()) {
        bad("No drum kits found (looked in " + paths::kits() + ").");
        tip("Run install.sh from the full Barline package (the one with the kits folder).");
    } else
        good(std::to_string(kits.size()) + " drum kits (" + paths::kits() + ")");

    head("The looper pedal");
    auto cards = snd::cards();
#ifdef __linux__
    if (!exists("/dev/snd")) {
        bad("This computer has no sound system Barline can see (/dev/snd is missing).");
    } else
#endif
    if (cards.empty()) {
        bad("No sound devices showing.");
    }
    // permission to use the sound devices (Void and others without a login manager: the audio group)
    bool denied = false;
#ifdef __linux__
    if (DIR *d = opendir("/dev/snd")) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string p = std::string("/dev/snd/") + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISCHR(st.st_mode) && access(p.c_str(), R_OK | W_OK) != 0) denied = true;
        }
        closedir(d);
    }
#endif
    if (denied) {
        bad("You don't have permission to use some sound devices.");
        tip("Add yourself to the audio group, then log out and back in:");
        tip("  sudo usermod -aG audio $USER");
    }
    config::Config cfg = config::load();
    app::Setup setup = app::find_devices(cfg);
    const snd::Card *looper = nullptr;
    for (const auto &c : cards)
        if (setup.looper && c.index == setup.dev.midi_card) looper = &c;
    if (!looper)
        for (const auto &c : cards) {
            std::string n = c.name + " " + c.id;
            for (auto &ch : n) ch = (char)std::tolower((unsigned char)ch);
            if (n.find("looper") != std::string::npos) looper = &c;
        }
    if (!looper) {
        bad("The pedal isn't showing. Plug it in with USB (and switch it on), then run this again.");
        tip("If it's plugged in: on the pedal, Menu > USB should NOT be in drive mode (EXIT, then RESTART it).");
#ifdef _WIN32
        tip("Windows 10 (1703 or later) and 11 have the USB audio driver it needs built in.");
#endif
    } else {
        good("Found it: " + looper->name + " (sound card " + std::to_string(looper->index) + ")");
        if (looper->play.empty()) bad("It has no sound input showing, so the drums can't reach it.");
        if (looper->midi.empty()) {
            bad("It has no MIDI showing: on the pedal, Menu > MIDI > Source: USB.");
        }
        if (!in_use) {
            snd::Pcm p;
            std::string err;
            if (!looper->play.empty()) {
                if (p.open(looper->index, looper->play[0], false, 44100, 2, 128, 2, &err)) {
                    good("Drums to the pedal: works (" + p.format + ", " + std::to_string(p.rate) + " Hz, " + std::to_string(p.period) + "-frame blocks)");
                    p.close();
                } else {
                    bad("Drums to the pedal: " + err);
                    if (err.find("another program") != std::string::npos && wireplumber) {
                        tip("PipeWire (this computer's sound server) is holding the pedal. To have it leave the");
                        tip("pedal to Barline for good, run:  barline doctor --fix");
                    }
                }
            }
            if (!looper->midi.empty()) {
                snd::Midi m;
                if (m.open(looper->index, looper->midi[0], false, true, &err)) good("MIDI to the pedal: works");
                else bad("MIDI to the pedal: " + err);
            }
        }
        tip("On the pedal (hold the encoder for the menu): MIDI > Source USB, Clock On, Ext Ctrl On,");
        tip("Channel Omni; Logic R>P>D.");
    }

    head("This computer's own sound (the headphone click, Hear on laptop)");
    if (setup.dev.click_card < 0) maybe("No separate laptop output found; the click can go out with the drums instead.");
    else if (!in_use) {
        snd::Pcm p;
        std::string err;
        bool ok = false;
        for (unsigned rate : {44100u, 48000u})
            if (!ok && p.open(setup.dev.click_card, setup.dev.click_dev, false, rate, 2, 256, 3, &err)) ok = true;
        if (ok) good("Works: " + setup.click_name);
        else {
            maybe(setup.click_name + ": " + err);
            if (sound_server && err.find("another program") != std::string::npos) tip("Another program has it right now; Barline keeps trying every few seconds (or close what's playing).");
        }
    }
    if (sound_server) tip("(PipeWire or PulseAudio is running: fine. Barline talks to the devices directly.)");

    head("Steady timing");
#ifndef __linux__
    good("Barline asks the system for audio priority for its sound and MIDI.");
#else
    rlimit rl{};
    if (getrlimit(RLIMIT_RTPRIO, &rl) == 0 && rl.rlim_cur > 0) good("Barline can run its sound at high priority.");
    else {
        maybe("Optional: Barline can't ask for high priority, so a busy computer could hiccup.");
        tip("For rock-steady timing, add this line to /etc/security/limits.conf, then log out and in:");
        tip("  @audio - rtprio 95      (and be in the audio group: sudo usermod -aG audio $USER)");
    }
#endif

    head("The iPad / phone page");
    if (!cfg.web) maybe("Turned off (web = no in " + config::path() + ").");
    else {
#ifndef _WIN32
        if (!in_use && !port_free(cfg.web_port)) bad("Port " + std::to_string(cfg.web_port) + " is taken by another program (Barline will use the next free one).");
#endif
        net::Net shown = net::pick(cfg.network);
        std::string ip = shown.ip;
        if (ip.empty() || ip == "127.0.0.1") {
            if (cfg.network == "auto") bad("This computer isn't on a network. Join the same Wi-Fi as the iPad, plug in a router, or turn on a hotspot.");
            else bad("The network picked for the page (" + cfg.network + ") isn't there. Plug it in, or set Network back to Auto.");
        } else
            good("Open http://" + ip + ":" + std::to_string(cfg.web_port) + " on the iPad (on " + shown.label() + "), code " + cfg.web_code);
        if (cfg.network_only) tip("Only devices on " + shown.label() + " get the page (network_only = on).");
        // a router of your own, plugged into a USB port (through a USB network adapter, or a router that
        // shares its network over USB): the iPad joins that router's Wi-Fi, no venue Wi-Fi needed
        bool any_usb = false;
        for (const auto &n : net::list()) {
            if (n.kind != "usb") continue;
            any_usb = true;
            if (!n.ip.empty()) good("USB router: " + n.name + " is on, at " + n.ip + ". Put the iPad on that router's Wi-Fi.");
            else if (!n.up) {
                maybe("USB router: " + n.name + " is plugged in but no link yet. Is the router on, and the cable in one of its LAN ports");
                tip("  (not the WAN/Internet port)? A travel router can take a minute to start.");
            } else {
                bad("USB router: " + n.name + " has a link but no address. The router hands one out (DHCP); ask for it:");
#ifdef _WIN32
                tip("  Settings > Network & internet > Ethernet: set it to get an address automatically (DHCP).");
#elif defined(__APPLE__)
                tip("  System Settings > Network > that adapter > Details > TCP/IP: Configure IPv4 Using DHCP.");
#endif
                if (!in_path("nmcli").empty()) tip("  nmcli device connect " + n.name);
                else if (!in_path("dhcpcd").empty()) tip("  sudo dhcpcd " + n.name + "      (Void: sudo ln -s /etc/sv/dhcpcd /var/service/ does every network for good)");
                else if (!in_path("dhclient").empty()) tip("  sudo dhclient " + n.name);
                else if (!in_path("udhcpc").empty()) tip("  sudo udhcpc -i " + n.name);
                tip("  If the router doesn't hand out addresses, turn on its DHCP server (its own setup page).");
            }
        }
        if (!any_usb) tip("Tip: no venue Wi-Fi? Plug a travel router into a USB port (a USB network adapter, or the router's USB sharing) and put the iPad on its Wi-Fi.");
        std::string host = sys::host_name();
#ifdef __APPLE__
        bool mdns = true;
#elif defined(_WIN32)
        bool mdns = false;
#else
        bool mdns = exists("/run/avahi-daemon/pid") || exists("/var/run/avahi-daemon/pid");
#endif
        if (mdns && !host.empty())
            good("Bookmark-friendly: http://" + host + ".local:" + std::to_string(cfg.web_port) + " works on any network");
#ifdef __linux__
        else
            tip("Tip: with Avahi running, the iPad can use this-computer's-name.local, which never changes between networks.");
#endif
#ifdef _WIN32
        tip("The first time Barline starts, Windows asks to let it on the network: allow Private networks.");
#elif defined(__APPLE__)
        tip("If macOS asks whether Barline may accept incoming network connections, say Allow.");
#endif
        if (exists("/etc/ufw/ufw.conf")) {
            std::ifstream f("/etc/ufw/ufw.conf");
            std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (all.find("ENABLED=yes") != std::string::npos) {
                maybe("The firewall (ufw) is on. If the iPad can't connect:  sudo ufw allow " + std::to_string(cfg.web_port) + "/tcp");
            }
        }
        if (!in_path("firewall-cmd").empty()) maybe("If the iPad can't connect, let it through the firewall:  sudo firewall-cmd --add-port=" + std::to_string(cfg.web_port) + "/tcp");
    }

    head("The pedal's own menu (hold the encoder; Barline can't read these, so check them once)");
    tip("MIDI > Ext Ctrl: On    <- off, the pedal ignores every press Barline sends (loops never start)");
    tip("MIDI > Source: USB    Clock: On    Channel: Omni (or the channel in Barline's settings)");
    tip("Clock off: a press flashes Rec at 0:00 and waits. Track B needs Multi, Sync, or Song mode,");
    tip("set on the pedal (Menu > Mode), and the same in Barline's Pedal mode.");

    head("The window (barline window)");
#ifdef _WIN32
    good("Opens as an app window in Microsoft Edge (or Chrome).");
#elif defined(__APPLE__)
    good("Opens as an app window in Chrome, Edge, or Brave if you have one; otherwise in Safari.");
#else
    std::string browser;
    for (const char *b : {"chromium", "chromium-browser", "google-chrome-stable", "google-chrome", "brave-browser", "brave", "microsoft-edge", "vivaldi-stable", "vivaldi"})
        if (browser.empty() && !in_path(b).empty()) browser = b;
    if (!browser.empty()) good("Opens as an app window in " + browser + ".");
    else if (!in_path("firefox").empty()) good("Opens in a Firefox window (with Chromium installed it'd be a plain app window).");
    else if (!in_path("xdg-open").empty()) maybe("Opens in your default browser.");
    else bad("No web browser found, so the window can't open (the terminal screen and the iPad still work).");
#endif

    std::printf("\n%s\n", problems ? (std::to_string(problems) + (problems == 1 ? " thing" : " things") + " to look at (marked !!).").c_str()
                                    : "All good. Play on.");
    return problems ? 1 : 0;
}

}  // namespace doctor
