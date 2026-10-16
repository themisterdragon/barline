// The live screen in the terminal, and the actions shared with the iPad page.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "engine.h"
#include "net.h"
#include "songs.h"

namespace app {

// The devices found for this setup (and why)
struct Setup {
    engine::Devices dev;
    std::string out_name, click_name, midi_name;   // what was picked, in words
    bool looper = false;                           // the pedal is plugged in
    std::vector<std::string> notes;                // what to know ("no MIDI: the pedal won't follow")
};
Setup find_devices(const config::Config &c);

// Everything Barline can be asked to do, by name, from the keyboard, a footswitch, MIDI, or the
// iPad page. Thread-safe.
class Control {
public:
    Control(engine::Engine &e, config::Config &c, songs::Library &lib);
    bool act(const std::string &action, const std::string &arg = "");
    // the state for screens: a few lines of JSON for the iPad, or read fields directly
    std::string json();          // what's happening (small: the iPad asks often)
    std::string lists_json();    // the beats, kits, and favorites (asked once in a while)
    config::Config &cfg() { return cfg_; }
    engine::Engine &engine() { return e_; }
    std::mutex &mu() { return mu_; }
    void load_kit(const std::string &slug);
    void apply_song(int i);                    // the i-th song of the set list in use
    void apply(const songs::Song &s);          // any song
    std::vector<songs::Song> set_songs();       // the set list in use (or every song, A-Z, when there's none)
    songs::Library &library() { return lib_; }
    std::string current_song;                   // its id ("" = none)
    // the iPad page: where it is ("" = off). The address follows the network (a venue's Wi-Fi, a hotspot).
    std::string address();
    std::string bookmark();                     // a name that stays put across networks (name.local), or ""
    net::Net network();                         // the network the address is on (Wi-Fi, a USB router...)
    void find_network();                        // look again (the watch does this every 2 s)
    int web_port = 0;
    bool lan = true;                            // other devices may use the page (else only this computer's window)
    // this computer's own window: a key made fresh each run (never saved), so only the window
    // Barline opens gets in without the pairing code
    std::string local_token;
    std::string window_url();                   // "" when the page is off
    bool window_mode = false;                   // running as a window (no terminal screen): closing it quits
    void saw_window();                          // the window asked for the state (it's open)
    void window_bye();                          // the window is closing (or reloading: it'll be back in a moment)
    bool window_closed();                       // it said bye and didn't come back
    // the devices in use, and a watch on them: the pedal plugged in later, unplugged, or back
    Setup setup();
    void set_setup(const Setup &s);
    void watch_devices();                       // starts the watch (a background thread)
    void stop_watching();
    bool preview = false;                       // drums on the laptop's speakers too (never saved; off at start)
    void push_settings();
    std::vector<std::string> recent;   // what happened, newest last
    void note(const std::string &s);
    bool has_device(const std::string &token);   // a paired iPad/phone
    void add_device(const std::string &token);
    std::atomic<bool> quit{false};
    // how long loops on a track are: B has its own when set (Song mode lets them differ)
    int bars_for(int t) const { return t == 1 && cfg_.loop_bars_b > 0 ? cfg_.loop_bars_b : cfg_.loop_bars; }
    int track = 0;                 // the loop track the buttons work on (0 = A, 1 = B)

private:
    engine::Engine &e_;
    config::Config &cfg_;
    songs::Library &lib_;
    std::mutex mu_;                  // recent, setup_, the address
    Setup setup_;
    std::string address_, bookmark_;
    net::Net net_;
    std::thread watch_;
    std::atomic<bool> watching_{false};
    std::atomic<long long> window_seen_{0}, window_bye_{0};   // ms on the steady clock
    std::recursive_mutex act_mu_;    // cfg_ (the keyboard and the iPad both act)
    // a beat being edited on the iPad: what to go back to when it's done
    bool editing_ = false, edit_preview_on_ = false, edit_started_ = false;
    std::string edit_back_style_;
    bool laptop_was_ = false;   // the laptop's sound was wanted last time (so "busy" is said once)
};

int run(Control &ctl);
void log_line(const std::string &s);   // into barline.log (with the time)
// what Barline is set up as, for the screens: "Pedal connected" etc.
std::string connection(const Setup &s, bool healthy);

}  // namespace app
