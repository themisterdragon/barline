#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#include "paths.h"
#include "sys.h"

namespace config {

const std::map<std::string, std::string> &default_keys() {
    // page-turner footswitches send Page Down / Page Up (or the arrows), so those do the most
    static const std::map<std::string, std::string> K = {
        {"play", "space"},      {"next_part", "n"},     {"end", "e"},           {"tap", "t"},
        {"tempo_up", "+"},      {"tempo_down", "-"},    {"record", "r"},        {"overdub", "o"},
        {"smart", "pgdn"},      {"undo", "u"},          {"undo2", "pgup"},      {"clear", "c"},
        {"stop_loops", "s"},    {"track_b", "b"},       {"next_song", "right"}, {"prev_song", "left"},
        {"bars_up", "]"},       {"bars_down", "["},     {"kits", "k"},          {"beats", "p"},
        {"songs", "l"},         {"settings", ","},      {"help", "?"},          {"quit", "q"},
        {"panic", "!"},         {"click_toggle", "m"},  {"all_beats", "a"},     {"builder", "j"},       {"preview", "h"},
        {"swing_step", "w"},    {"pocket_down", "<"},   {"pocket_up", ">"},     {"loose_down", "("},    {"loose_up", ")"},      {"lock", "g"},
        {"window", "v"},        {"pause_half", "z"},    {"pause_bar", "x"},     {"clap", "d"},
        {"record_ab", "f"}};
    return K;
}

std::string path() { return paths::data() + "/barline.conf"; }

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

static std::string new_code() {
    std::random_device rd;
    std::string c;
    for (int i = 0; i < 6; i++) c += (char)('0' + rd() % 10);
    return c;
}

Config load() {
    Config c;
    c.keys = default_keys();
    std::ifstream f(path());
    Song *song = nullptr;
    for (std::string line; std::getline(f, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "[song]") {
            c.songs.push_back(Song());
            song = &c.songs.back();
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        double d = std::atof(v.c_str());
        if (song) {
            if (k == "name") song->name = v;
            else if (k == "style") song->style = v;
            else if (k == "kit") song->kit = v;
            else if (k == "bpm") song->bpm = d;
            else if (k == "loop_bars") song->loop_bars = (int)d;
            else if (k == "slot") song->slot = (int)d;
            else if (k == "count_in") song->count_in = (int)d;
            else if (k == "notes") song->notes = v;
            continue;
        }
        if (k == "out_device") c.out_device = v;
        else if (k == "click_device") c.click_device = v;
        else if (k == "midi_device") c.midi_device = v;
        else if (k == "period") c.period = (int)d;
        else if (k == "style") c.style = v;
        else if (k == "kit") c.kit = v;
        else if (k == "bpm") c.bpm = d;
        else if (k == "loop_bars") c.loop_bars = (int)d;
        else if (k == "loop_bars_b") c.loop_bars_b = std::max(0, std::min(64, (int)d));
        else if (k == "count_in") c.count_in = (int)d;
        else if (k == "volume") c.volume = d;
        else if (k == "click_volume") c.click_volume = d;
        else if (k == "humanize") c.humanize = d;
        else if (k == "swing") c.swing = d;
        else if (k == "click") c.click = v == "on";
        else if (k == "click_always") c.click_always = v == "on";
        else if (k == "send_clock") c.send_clock = v == "on";
        else if (k == "midi_start") c.midi_start = v == "on";
        else if (k == "clock_div") c.clock_div = std::max(0, std::min(4, (int)d));
        else if (k == "midi_channel") c.midi_channel = (int)d;
        else if (k == "lead_ms") c.lead_ms = d;
        else if (k == "pocket_ms") c.pocket_ms = d;
        else if (k == "locked") c.locked = v == "on";
        else if (k == "grace_beats") c.grace_beats = d;
        else if (k == "pedal_mode") c.pedal_mode = v;
        else if (k == "latency_ms") c.latency_ms = d;
        else if (k == "play_early_ms") c.play_early_ms = std::max(0.0, std::min(200.0, d));
        else if (k == "pedal_lag_ms") c.pedal_lag_ms = std::max(0.0, std::min(200.0, d));
        else if (k == "web") c.web = v == "on";
        else if (k == "web_port") c.web_port = (int)d;
        else if (k == "network") c.network = v.empty() ? "auto" : v;
        else if (k == "network_only") c.network_only = v == "on";
        else if (k == "web_code") c.web_code = v;
        else if (k == "song") c.song = (int)d;
        else if (k == "favorite") c.favorites.push_back(v);
        else if (k == "setlist") c.setlist = v;
        else if (k == "device") c.devices.push_back(v);
        else if (k.compare(0, 4, "key.") == 0) c.keys[k.substr(4)] = v;
    }
    if (c.web_code.size() != 6) c.web_code = new_code();

    return c;
}

bool save(const Config &c) {
    std::ostringstream o;
    auto onoff = [](bool b) { return b ? "on" : "off"; };
    o << "# Barline settings (changes here take effect the next time it starts)\n";
    o << "out_device = " << c.out_device << "\nclick_device = " << c.click_device << "\nmidi_device = " << c.midi_device << "\n";
    o << "period = " << c.period << "\nstyle = " << c.style << "\nkit = " << c.kit << "\nbpm = " << c.bpm << "\n";
    o << "loop_bars = " << c.loop_bars << "\nloop_bars_b = " << c.loop_bars_b << "\ncount_in = " << c.count_in << "\nvolume = " << c.volume << "\nclick_volume = " << c.click_volume
      << "\n";
    o << "humanize = " << c.humanize << "\nswing = " << c.swing << "\nclick = " << onoff(c.click) << "\nclick_always = " << onoff(c.click_always)
      << "\n";
    o << "pocket_ms = " << c.pocket_ms << "\nlocked = " << onoff(c.locked) << "\ngrace_beats = " << c.grace_beats << "\n";
    o << "pedal_mode = " << c.pedal_mode << "\n";
    o << "clock_div = " << c.clock_div << "\n";
    o << "send_clock = " << onoff(c.send_clock) << "\nmidi_start = " << onoff(c.midi_start) << "\nmidi_channel = " << c.midi_channel << "\nlead_ms = " << c.lead_ms
      << "\nlatency_ms = " << c.latency_ms << "\nplay_early_ms = " << c.play_early_ms << "\npedal_lag_ms = " << c.pedal_lag_ms << "\n";
    o << "network = " << c.network << "\nnetwork_only = " << onoff(c.network_only) << "\n";
    o << "web = " << onoff(c.web) << "\nweb_port = " << c.web_port << "\nweb_code = " << c.web_code << "\nsong = " << c.song << "\n";
    for (const auto &f : c.favorites) o << "favorite = " << f << "\n";
    o << "setlist = " << c.setlist << "\n";
    for (const auto &d : c.devices) o << "device = " << d << "\n";
    for (const auto &k : c.keys)
        if (default_keys().count(k.first) && default_keys().at(k.first) != k.second) o << "key." << k.first << " = " << k.second << "\n";
    for (const auto &s : std::vector<Song>()) {
        o << "\n[song]\nname = " << s.name << "\nstyle = " << s.style << "\nkit = " << s.kit << "\nbpm = " << s.bpm << "\nloop_bars = " << s.loop_bars
          << "\nslot = " << s.slot << "\ncount_in = " << s.count_in << "\n";
        if (!s.notes.empty()) o << "notes = " << s.notes << "\n";
    }
    // write to a new file, then swap it in, so a crash never leaves half a file
    return sys::write_private(path(), o.str());
}

}  // namespace config
