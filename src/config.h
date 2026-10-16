// What Barline remembers: settings (devices by name, tempo, beat, kit, loop length...) and set
// lists. Plain text files in ~/.local/share/barline, so they're easy to read and back up.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace config {

struct Song {
    std::string name;
    std::string style = "pop";
    std::string kit = "virtuosity";
    double bpm = 100;
    int loop_bars = 4;
    int slot = -1;        // the pedal's saved loop slot to load (-1 = none)
    int count_in = 1;
    std::string notes;    // "capo 2, start with chorus"
};

struct Config {
    // devices, remembered by name (card numbers change between boots)
    std::string out_device = "auto";     // "auto" = the pedal if it's plugged in, else this computer's speakers
    std::string click_device = "auto";   // "auto" = this computer's own headphone jack; "none"; "drums" = mixed in
    std::string midi_device = "auto";
    int period = 128;
    // the live setup
    std::string style = "pop", kit = "virtuosity";
    double bpm = 100;
    int loop_bars = 4;
    int loop_bars_b = 0;        // track B's loops (0 = the same as A): Song mode lets them differ
    int count_in = 1;
    double volume = 0.8, click_volume = 0.6, humanize = 0.25, swing = -1;
    bool click = false, click_always = false, send_clock = true, midi_start = false;
    int clock_div = 1;          // MIDI clock: quarter notes per pedal beat (1 normal; 2 half bars; 0 a whole bar)   // the headphone click is optional
    int midi_channel = 1;
    double lead_ms = 30, latency_ms = 0;
    double play_early_ms = 30;  // Play on a stopped loop goes this early (the pedal waits for the downbeat)
    double pedal_lag_ms = 0;    // delay MIDI this much so the pedal's downbeats land with the USB drums
                                // (to be measured with a clean test loop; latency_ms is anything extra on top)
    double pocket_ms = 0;       // the feel: + laid back, - pushed
    bool locked = false;        // Feel mode: locked to the grid (no pocket, no looseness) or human
    double grace_beats = 0;     // (no longer used since 0.5.2: a loop always starts on a bar line)
    std::string pedal_mode = "off";   // the mode the pedal is set to (Menu > Mode): off (not said), single, multi, sync, song
    // the iPad / phone page
    bool web = true;
    int web_port = 8580;
    // the network the page is shown on: auto (a router plugged into a USB port first, then a cable,
    // then the network the computer goes out on), usb, wired, wifi, or a network's name ("enx00e04c...")
    std::string tui_theme = "stage";      // the laptop screen's look (Stage, Classic, Neon Grid, ...)
    std::string tui_lights = "theme";     // its beat lights: the theme's own, or blocks, dots, leds, numbers, pips, sweep
    std::string network = "auto";
    bool network_only = false;            // only devices on that network get the page (not the venue's Wi-Fi)
    std::string web_code;                 // the pairing code (made on first run)
    // keys: action -> key name ("space", "pgdn", "r", ...)
    std::map<std::string, std::string> keys;
    std::vector<std::string> favorites;   // beat ids, in the order starred
    std::string setlist;                  // the set list in use ("" = none)
    int song = -1;                        // the position in it (-1 = none)
    std::vector<std::string> devices;     // the iPad/phone pairings (random tokens)
    std::vector<Song> songs;              // (old settings files kept songs here; moved to songs/ on start)
};

Config load();
bool save(const Config &c);
std::string path();
const std::map<std::string, std::string> &default_keys();

}  // namespace config
