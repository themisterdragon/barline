#include "app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <fstream>
#include <ctime>
#include <thread>


#include "http.h"
#include "kit.h"
#include "paths.h"
#include "pattern.h"
#include "snd.h"
#include "sys.h"
#include "term.h"
#include "version.h"

namespace app {

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ----------------------------------------------------------------------------- devices

static bool is_looper(const snd::Card &c) {
    std::string n = lower(c.name + " " + c.id);
    for (const char *w : {"looper"})
        if (n.find(w) != std::string::npos) return true;
    return false;
}

static const snd::Card *by_name(const std::vector<snd::Card> &cards, const std::string &want) {
    for (const auto &c : cards)
        if (lower(c.id) == lower(want) || lower(c.name) == lower(want)) return &c;
    return nullptr;
}

Setup find_devices(const config::Config &cfg) {
    Setup s;
    auto cards = snd::cards();
    const snd::Card *looper = nullptr, *own = nullptr;
    for (const auto &c : cards) {
        if (is_looper(c)) looper = &c;
        else if (!own && !c.play.empty() && lower(c.name).find("hdmi") == std::string::npos) own = &c;   // the built-in sound
    }
    s.looper = looper != nullptr;
    // the drums
    // the drums go to the pedal only; the laptop's own speakers only with "hear on laptop"
    const snd::Card *out = cfg.out_device == "auto" ? (looper && !looper->play.empty() ? looper : nullptr) : by_name(cards, cfg.out_device);
    if (out && !out->play.empty()) {
        s.dev.out_card = out->index;
        s.dev.out_dev = out->play[0];
        s.out_name = out->name;
    } else
        s.notes.push_back("Drums are silent until the pedal is plugged in (or turn on Hear on laptop).");
    // the click, in your headphones
    if (cfg.click_device == "none") {
    } else if (cfg.click_device == "drums") {
        s.click_name = "with the drums";
    } else {
        const snd::Card *ck = cfg.click_device == "auto" ? own : by_name(cards, cfg.click_device);
        if (ck && !ck->play.empty() && ck != out) {
            s.dev.click_card = ck->index;
            s.dev.click_dev = ck->play[0];
            s.click_name = ck->name;
        } else
            s.click_name = "off (no laptop output)";
    }
    // MIDI to the pedal
    const snd::Card *m = cfg.midi_device == "auto" ? looper : by_name(cards, cfg.midi_device);
    if (m && !m->midi.empty()) {
        s.dev.midi_card = m->index;
        s.dev.midi_dev = m->midi[0];
        s.midi_name = m->name;
    } else
        s.notes.push_back(looper ? "The pedal has no MIDI device showing: set its MIDI Source to USB." :
                                   "The pedal isn't plugged in: drums only, nothing sent to the pedal.");
    s.dev.period = (unsigned)std::max(64, std::min(2048, cfg.period));
    return s;
}

static bool same_devices(const engine::Devices &a, const engine::Devices &b) {
    return a.out_card == b.out_card && a.out_dev == b.out_dev && a.click_card == b.click_card && a.click_dev == b.click_dev &&
           a.midi_card == b.midi_card && a.midi_dev == b.midi_dev;
}

std::string connection(const Setup &s, bool healthy) {
    if (!s.looper) return "Pedal not plugged in";
    if (s.dev.midi_card < 0) return "Pedal (no MIDI)";
    return healthy ? "Pedal connected" : "Pedal not answering";
}

Setup Control::setup() {
    std::lock_guard<std::mutex> lock(mu_);
    return setup_;
}

void Control::set_setup(const Setup &s) {
    std::lock_guard<std::mutex> lock(mu_);
    setup_ = s;
}

std::string Control::address() {
    std::lock_guard<std::mutex> lock(mu_);
    return address_;
}

std::string Control::bookmark() {
    std::lock_guard<std::mutex> lock(mu_);
    return bookmark_;
}

net::Net Control::network() {
    std::lock_guard<std::mutex> lock(mu_);
    return net_;
}

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Control::saw_window() { window_seen_ = now_ms(); }
void Control::window_bye() { window_bye_ = now_ms(); }
bool Control::window_closed() {
    long long bye = window_bye_;
    return bye && window_seen_ < bye && now_ms() - bye > 4000;
}

std::string Control::window_url() {
    if (!web_port) return "";
    return "http://127.0.0.1:" + std::to_string(web_port) + "/#local=" + local_token;
}

// the address: this computer's on the network picked (a USB router, a cable, the Wi-Fi), and its
// name.local when the computer answers to that name (Avahi running), which an iPad can bookmark once
// for every network
void Control::find_network() {
    if (!web_port) return;
    std::string want;
    {
        std::lock_guard<std::recursive_mutex> guard(act_mu_);
        want = cfg_.network;
    }
    net::Net n = net::pick(want);
    std::string address = "http://" + (n.ip.empty() ? std::string("127.0.0.1") : n.ip) + ":" + std::to_string(web_port);
    std::string bookmark;
    // name.local answers when the computer advertises it: a Mac always does; Linux with Avahi running
#ifdef __APPLE__
    bool mdns = true;
#elif defined(_WIN32)
    bool mdns = false;
#else
    bool mdns = sys::exists("/run/avahi-daemon/pid") || sys::exists("/var/run/avahi-daemon/pid");
#endif
    std::string h = sys::host_name();
    if (mdns && !h.empty()) bookmark = "http://" + h + ".local:" + std::to_string(web_port);
    std::lock_guard<std::mutex> lock(mu_);
    if (address != address_ && !address_.empty()) {
        if (n.ip.empty()) recent.push_back(n.kind == "usb" ? "No USB router with an address yet (plugged in? see barline doctor)" : "The iPad page's network isn't there");
        else recent.push_back("The iPad page moved to " + address + " (" + n.label() + ")");
    }
    address_ = address;
    bookmark_ = bookmark;
    net_ = n;
}

void Control::watch_devices() {
    if (watching_.exchange(true)) return;
    find_network();
    watch_ = std::thread([this] {
        auto last_retry = std::chrono::steady_clock::now(), last_laptop = last_retry;
        int retries = 0;
        while (watching_) {
            for (int i = 0; i < 20 && watching_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!watching_) break;
            Setup now;
            {
                std::lock_guard<std::recursive_mutex> guard(act_mu_);
                now = find_devices(cfg_);
            }
            Setup cur = setup();
            bool changed = !same_devices(now.dev, cur.dev);
            bool broken = !e_.healthy();
            // the laptop's sound (click / hear on laptop) wanted but busy: try again now and then (no gap in the drums)
            if (!changed && !broken && e_.laptop_missing() && std::chrono::steady_clock::now() - last_laptop > std::chrono::seconds(5)) {
                last_laptop = std::chrono::steady_clock::now();
                if (e_.set_laptop(true)) note("The laptop's sound is working now.");
            }
            if (!changed && broken) {
                // asked-for devices that are listed but won't open or answer: try again, less often as it goes on
                auto since = std::chrono::steady_clock::now() - last_retry;
                if (since < std::chrono::seconds(retries < 3 ? 2 : 10)) continue;
            }
            if (changed || broken) {
                last_retry = std::chrono::steady_clock::now();
                std::string err;
                bool ok = e_.reopen(now.dev, &err);
                retries = ok ? 0 : retries + 1;
                set_setup(now);
                if (now.looper && !cur.looper) note("The pedal is plugged in: drums and MIDI go to the pedal.");
                else if (!now.looper && cur.looper) note("The pedal was unplugged. The drums are silent until it's back; the bars keep counting.");
                else if (ok && broken) note("The sound and MIDI are working again.");
                if (!ok && retries == 1) note("Couldn't open " + err + " (trying again)");
            }
            find_network();
        }
    });
}

void Control::stop_watching() {
    watching_ = false;
    if (watch_.joinable()) watch_.join();
}

// ----------------------------------------------------------------------------- actions

Control::Control(engine::Engine &e, config::Config &c, songs::Library &lib) : e_(e), cfg_(c), lib_(lib) {
    e_.log = [this](const std::string &s) { note(s); };
}

void Control::note(const std::string &s) {
    std::lock_guard<std::mutex> lock(mu_);
    recent.push_back(s);
    if (recent.size() > 40) recent.erase(recent.begin());
    log_line(s);
    engine::trace("NOTE " + s);
}

// what happened, kept in barline.log too (for "what went wrong at the gig?"); a long log starts over
void log_line(const std::string &s) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    static std::string path = paths::data() + "/barline.log";
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (sys::file_size(path) > 512 * 1024) sys::replace_file(path, path + ".old");
    }
    char when[32];
    struct tm tmv;
    sys::local_time(time(nullptr), &tmv);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S ", &tmv);
    sys::append_private(path, when + s + "\n");
}

void Control::push_settings() {
    engine::Settings s;
    s.bpm = cfg_.bpm;
    s.swing = cfg_.swing;
    s.humanize = cfg_.locked ? 0 : cfg_.humanize;   // locked: exactly on the grid (the settings wait for Human)
    s.volume = cfg_.volume;
    s.click_volume = cfg_.click_volume;
    s.click = cfg_.click;
    s.click_always = cfg_.click_always;
    s.count_in = cfg_.count_in;
    s.midi_channel = cfg_.midi_channel;
    s.send_clock = cfg_.send_clock;
    s.midi_start = cfg_.midi_start;
    s.clock_div = cfg_.clock_div;
    s.lead_ms = cfg_.lead_ms;
    s.latency_ms = cfg_.latency_ms + cfg_.pedal_lag_ms;
    s.play_early_ms = cfg_.play_early_ms;
    s.click_in_drums = cfg_.click_device == "drums";
    s.preview = preview;
    s.pocket_ms = cfg_.locked ? 0 : cfg_.pocket_ms;
    s.grace_beats = cfg_.grace_beats;
    {
        static const char *M[] = {"off", "single", "multi", "sync", "song"};
        s.pedal_mode = 0;
        for (int i = 1; i < 5; i++)
            if (cfg_.pedal_mode == M[i]) s.pedal_mode = i;
    }
    e_.set_settings(s);
    // the laptop's own sound only while something wants it (else the computer's other sound can have it)
    std::string err;
    bool want = (cfg_.click && cfg_.click_device != "drums") || preview;
    if (!e_.set_laptop(want, &err) && want != laptop_was_) note("The laptop's sound is busy (" + err + "); Barline keeps trying.");
    laptop_was_ = want;
}

void Control::load_kit(const std::string &slug) {
    std::string err;
    auto k = kit::load(paths::kits(), slug, &err);
    if (!k) {
        note("Couldn't load the kit: " + err);
        return;
    }
    cfg_.kit = slug;
    e_.set_kit(k);
    note("Kit: " + k->name);
}

std::vector<songs::Song> Control::set_songs() {
    songs::SetList l;
    if (!cfg_.setlist.empty() && lib_.get_setlist(cfg_.setlist, l)) {
        std::vector<songs::Song> out;
        for (const auto &id : l.songs) {
            songs::Song s;
            if (lib_.get(id, s)) out.push_back(s);
        }
        return out;
    }
    return lib_.all();
}

void Control::apply(const songs::Song &s) {
    current_song = s.id;
    cfg_.bpm = s.bpm;
    if (pattern::find(s.style)) cfg_.style = s.style;
    cfg_.loop_bars = s.loop_bars;
    cfg_.loop_bars_b = s.loop_bars_b;
    cfg_.count_in = s.count_in;
    cfg_.swing = s.swing;
    cfg_.pocket_ms = s.pocket;
    if (s.loose >= 0) cfg_.humanize = s.loose;
    if (s.locked >= 0) cfg_.locked = s.locked == 1;
    push_settings();
    e_.set_style(cfg_.style);
    if (!s.kit.empty() && s.kit != cfg_.kit) load_kit(s.kit);
    if (s.slot >= 0) {
        e_.load_slot(s.slot);
        e_.cleared(-1);   // what's on a loaded slot isn't known: the first switch to a track stops it on a bar, then plays it
    }
    note("Song: " + s.name + (s.notes.empty() ? "" : "  (" + s.notes + ")"));
}

void Control::apply_song(int i) {
    auto list = set_songs();
    if (i < 0 || i >= (int)list.size()) return;
    cfg_.song = i;
    apply(list[(size_t)i]);
}

bool Control::has_device(const std::string &token) {
    std::lock_guard<std::recursive_mutex> guard(act_mu_);
    bool found = false;
    std::vector<std::string> keys = cfg_.devices;
    if (!local_token.empty()) keys.push_back(local_token);   // this computer's own window
    for (const auto &d : keys) {
        unsigned char diff = d.size() == token.size() ? 0 : 1;   // compared in constant time
        for (size_t i = 0; i < d.size() && i < token.size(); i++) diff |= (unsigned char)(d[i] ^ token[i]);
        if (!diff) found = true;
    }
    return found;
}

void Control::add_device(const std::string &token) {
    std::lock_guard<std::recursive_mutex> guard(act_mu_);
    cfg_.devices.push_back(token);
    if (cfg_.devices.size() > 8) cfg_.devices.erase(cfg_.devices.begin());   // the oldest pairing goes
    config::save(cfg_);
}

bool Control::act(const std::string &a, const std::string &arg) {
    std::lock_guard<std::recursive_mutex> guard(act_mu_);
    if (engine::tracing()) {
        engine::Status st = e_.status();
        engine::trace("ACT " + a + (arg.empty() ? "" : " '" + arg.substr(0, 60) + "'") + " | bar " + std::to_string(st.bar) + " beat " + std::to_string(st.beat) +
                      " sec " + std::to_string(st.section) + " track " + std::to_string(st.playing_track) + " pedal '" + st.pedal_state + "'");
    }
    double num = std::atof(arg.c_str());
    if (a == "play") {
        engine::Status st = e_.status();
        if (st.playing && !st.ending) e_.end();
        else e_.play();
    } else if (a == "start") e_.play();
    else if (a == "end") e_.end();
    else if (a == "stop") e_.stop_now();
    else if (a == "next_part") e_.next_part();
    else if (a == "pause_half" || a == "pause_bar") {   // a dramatic pause (half a bar, or a whole one)
        if (!e_.status().playing) return true;
        e_.pause(a == "pause_half");
        note(a == "pause_half" ? "Pause: the second half of the bar, back in on the one" : "Pause: one whole bar, back in on the one");
    } else if (a == "clap") {   // clap along: drums out, hand claps in (on, off, or flip)
        engine::Status st = e_.status();
        if (!st.playing) return true;
        bool on = arg == "on" ? true : arg == "off" ? false : !st.clapping;
        e_.clap(on ? 1 : 0);
        note(on ? "Clap along from the next bar (the drums drop out)" : "The drums come back in on the next bar");
    }
    else if (a == "verse") e_.go_to(pattern::VERSE);
    else if (a == "chorus") e_.go_to(pattern::CHORUS);
    else if (a == "tap") e_.tap_tempo();
    else if (a == "tempo_up" || a == "tempo_down" || a == "tempo" || a == "tempo_half" || a == "tempo_double") {
        // half / double: for a tempo that was counted twice as fast (or half as fast) as you feel it
        if (a == "tempo_half") cfg_.bpm = cfg_.bpm / 2;
        else if (a == "tempo_double") cfg_.bpm = cfg_.bpm * 2;
        else cfg_.bpm = a == "tempo" ? num : cfg_.bpm + (a == "tempo_up" ? 1 : -1);
        cfg_.bpm = std::max(30.0, std::min(300.0, cfg_.bpm));
        push_settings();
        std::string ps = e_.status().pedal_state;
        if (ps.rfind("playing", 0) == 0 || ps.rfind("recording", 0) == 0)
            note("Heads up: loops already on the pedal stay at their own speed (the drums are moving away from them).");
    } else if (a == "record") {
        e_.record_loop(bars_for(track), track);
    } else if (a == "record_ab") {   // A, then B straight on, then A plays
        e_.record_through(bars_for(0), bars_for(1));
    } else if (a == "overdub") e_.overdub(bars_for(track), track);
    else if (a == "smart") {   // one footswitch: the first loop, then layers on top
        engine::Status st = e_.status();
        if (!(track ? st.has_b : st.has_a))
            e_.record_loop(bars_for(track), track);
        else
            e_.overdub(bars_for(track), track);
    } else if (a == "undo" || a == "undo2") e_.press(pedal::UNDO, engine::NOW);
    else if (a == "clear") {
        e_.clear_track(track);
    } else if (a == "stop_loops") e_.stop_loops(engine::BAR);
    else if (a == "cancel") e_.cancel_loop();
    else if (a == "play_a" || a == "play_b") e_.play_track(a == "play_b" ? 1 : 0);
    else if (a == "select") {   // pick a track (A or B) for the loop buttons
        int t = arg == "B" || arg == "b" ? 1 : 0;
        // nothing goes to the pedal as a selection: there, selecting in Song mode stops the playing loop
        // without starting the other. Each loop press selects its own track a moment before it goes.
        // Song mode (or not said): picking a track plays its loop from the next bar line (the other one
        // stops), unless Barline knows it's empty: then it's just ready to record
        engine::Status st = e_.status();
        bool empty = t ? st.empty_b : st.empty_a;
        bool full = !empty;
        bool song = cfg_.pedal_mode == "song" || cfg_.pedal_mode == "off";
        int heading = st.next_track == 0 || st.next_track == 1 ? st.next_track : st.next_track == 2 ? -1 : st.playing_track;   // where it's going
        if (song && !empty && heading != t) {
            track = t;
            e_.play_track(t);
        } else if (t != track) {
            track = t;
            note(std::string("Loop buttons work on track ") + (track ? "B" : "A") +
                 (song && !full && st.playing_track >= 0 ? " (empty: Record starts its loop)" : ""));
        }
    } else if (a == "clock_div") {
        cfg_.clock_div = std::max(0, std::min(4, (int)num));
        push_settings();
        note("Pedal clock: " + std::string(cfg_.clock_div == 0 ? "one beat a bar" : cfg_.clock_div == 1 ? "every beat" : "one beat every " + std::to_string(cfg_.clock_div) + " beats"));
    } else if (a == "pedal_mode") {
        if (arg == "off" || arg == "single" || arg == "multi" || arg == "sync" || arg == "song") {
            cfg_.pedal_mode = arg;
            if (arg == "single") track = 0;
            push_settings();
            note(arg == "off" ? "Pedal mode: not said (track B works as in Sync/Song)" : "Pedal mode: " + arg);
        }
    } else if (a == "network") {   // auto, usb, wired, wifi, or a network's name
        bool known = arg == "auto" || arg == "usb" || arg == "wired" || arg == "wifi";
        for (const auto &n : net::list()) known = known || n.name == arg;
        if (known) {
            cfg_.network = arg;
            config::save(cfg_);
            find_network();
            net::Net n = network();
            note(n.ip.empty() ? "iPad page network: " + arg + " (not there yet)" : "iPad page on " + n.label() + ": " + address());
        }
    } else if (a == "look") {   // the laptop screen's look: "theme:neon" or "lights:leds"
        if (arg.rfind("theme:", 0) == 0) cfg_.tui_theme = arg.substr(6);
        else if (arg.rfind("lights:", 0) == 0) cfg_.tui_lights = arg.substr(7);
        config::save(cfg_);
    } else if (a == "network_only") {
        cfg_.network_only = arg == "on" ? true : arg == "off" ? false : !cfg_.network_only;
        config::save(cfg_);
        note(cfg_.network_only ? "Only devices on " + network().label() + " get the page" : "Any device on a private network nearby can ask for the page (it still needs the code)");
    } else if (a == "track_b") {
        return act("select", track ? "A" : "B");
    } else if (a == "fade") e_.press(pedal::FADE, engine::BAR);
    else if (a == "reverse") e_.press(pedal::REVERSE, engine::BAR);
    else if (a == "half") e_.press(pedal::SPEED, engine::BAR);
    else if (a == "pedal") e_.press((pedal::Cmd)(int)num, engine::NOW);
    else if (a == "pedal_bar") {   // tests: "NOTE OFFSET_MS", that note on the next bar line
        int nn = 0;
        double off = 0;
        std::sscanf(arg.c_str(), "%d %lf", &nn, &off);
        if (nn > 0 && nn < 128) e_.press_at_bar(nn, off);
    }
    else if (a == "slot") e_.load_slot((int)num);
    else if (a == "bars_up" || a == "bars_down" || a == "bars") {
        // the selected track's length (B's own only matters when the pedal lets them differ: Song mode)
        static const int B[] = {1, 2, 4, 8, 16};
        int cur = bars_for(track), i = 0;
        while (i < 4 && B[i] < cur) i++;
        int n = a == "bars" ? std::max(1, std::min(64, (int)num)) : B[std::max(0, std::min(4, i + (a == "bars_up" ? 1 : -1)))];
        if (track == 1) cfg_.loop_bars_b = n == cfg_.loop_bars ? 0 : n;
        else cfg_.loop_bars = n;
        note(std::string("Loops on ") + (track ? "B" : "A") + " will be " + std::to_string(n) + (n == 1 ? " bar" : " bars"));
    } else if (a == "kit") load_kit(arg);
    else if (a == "swing" || a == "swing_step") {
        if (a == "swing") cfg_.swing = num < 0 ? -1 : std::min(1.0, num);
        else {   // the beat's own, then 0, 25, 50, 66%
            static const double STEPS[] = {-1, 0, 0.25, 0.5, 0.66};
            size_t i = 0;
            for (size_t j = 0; j < 5; j++)
                if (std::fabs(STEPS[j] - cfg_.swing) < 0.02) i = (j + 1) % 5;
            cfg_.swing = STEPS[i];
        }
        push_settings();
        note(cfg_.swing < 0 ? "Swing: the beat's own" : "Swing: " + std::to_string((int)std::lround(cfg_.swing * 100)) + "%");
    } else if ((a.rfind("pocket", 0) == 0 || a.rfind("loose", 0) == 0) && cfg_.locked) {
        cfg_.locked = false;   // reaching for the feel: back to Human, then the change
        push_settings();
        note("Feel: human (your pocket and looseness)");
        return act(a, arg);
    } else if (a == "pocket" || a == "pocket_up" || a == "pocket_down") {
        cfg_.pocket_ms = a == "pocket" ? num : cfg_.pocket_ms + (a == "pocket_up" ? 3 : -3);
        cfg_.pocket_ms = std::max(-15.0, std::min(15.0, cfg_.pocket_ms));
        push_settings();
        int p = (int)std::lround(cfg_.pocket_ms);
        note(p == 0 ? "Pocket: right on the beat" : p > 0 ? "Pocket: laid back " + std::to_string(p) + " ms" : "Pocket: pushed " + std::to_string(-p) + " ms");
    } else if (a == "loose" || a == "loose_up" || a == "loose_down") {
        cfg_.humanize = a == "loose" ? num : cfg_.humanize + (a == "loose_up" ? 0.1 : -0.1);
        cfg_.humanize = std::max(0.0, std::min(1.0, cfg_.humanize));
        push_settings();
        note("Looseness: " + std::to_string((int)std::lround(cfg_.humanize * 100)) + "%");
    } else if (a == "lock") {   // Feel mode: locked to the grid, or human (on, off, or flip)
        cfg_.locked = arg == "on" ? true : arg == "off" ? false : !cfg_.locked;
        push_settings();
        note(cfg_.locked ? "Feel: LOCKED to the grid (every hit exactly on time)" : "Feel: human (your pocket and looseness)");
    } else if (a == "save_feel") {   // the feel as it is now, into the song that's on
        songs::Song s;
        if (!current_song.empty() && lib_.get(current_song, s)) {
            s.swing = cfg_.swing;
            s.pocket = cfg_.pocket_ms;
            s.loose = cfg_.humanize;
            s.locked = cfg_.locked ? 1 : 0;
            s.bpm = cfg_.bpm;
            s.kit = cfg_.kit;
            s.estimated = false;   // you've played it and saved it: checked
            s.confirmed = true;
            s.style = cfg_.style;
            lib_.save(s);
            note("Saved the tempo, beat, kit, and feel to \"" + s.name + "\"");
        } else
            note("Pick a song first (the feel is saved to the song that's on).");
    } else if (a == "preview") {   // the drums on the laptop's speakers too: on, off, or flip
        preview = arg == "on" ? true : arg == "off" ? false : !preview;
        push_settings();
        note(preview ? "Hear on laptop: ON (drums on this computer's speakers too)" : "Hear on laptop: off");
        if (preview && !e_.has_laptop()) note("This computer has no sound output Barline can use.");
    } else if (a == "favorite") {   // star or unstar a beat
        auto it = std::find(cfg_.favorites.begin(), cfg_.favorites.end(), arg);
        if (it == cfg_.favorites.end()) {
            if (pattern::find(arg)) cfg_.favorites.push_back(arg);
        } else
            cfg_.favorites.erase(it);
    } else if (a == "noop") {
    }
    else if (a == "edit_preview") {   // a beat being edited on the iPad (arg: its .beat text) plays as it is now
        pattern::Style st;
        if (!pattern::from_text(arg, st)) return false;
        if (!editing_) {
            editing_ = true;
            edit_back_style_ = cfg_.style;
        }
        pattern::preview(st);
        e_.set_style("edit-preview");
    } else if (a == "edit_play") {   // play/stop the beat being edited; you asked to hear it, so on the laptop too
        if (!editing_) return false;
        if (e_.status().playing) e_.stop_now();
        else {
            if (!preview) {
                preview = true;
                edit_preview_on_ = true;
            }
            if (num >= 30 && num <= 300) cfg_.bpm = num;
            push_settings();
            e_.play();
            edit_started_ = true;
        }
    } else if (a == "edit_part") {   // the part being edited plays on repeat (a fill or ending with its groove before it)
        if (!editing_) return false;
        int sec = pattern::part_by_key(arg);
        e_.audition(sec <= pattern::VERSE ? -1 : sec);
    } else if (a == "edit_done") {   // leaving the editor: back to the beat that was on (or arg: the one just saved)
        if (!editing_) return true;
        if (edit_started_ && e_.status().playing) e_.stop_now();   // a preview doesn't keep going
        if (edit_preview_on_) preview = false;
        e_.audition(-1);
        std::string back = pattern::find(arg) ? arg : edit_back_style_;
        if (!pattern::find(back)) back = "rock";
        cfg_.style = back;
        push_settings();
        e_.set_style(back);
        editing_ = edit_preview_on_ = edit_started_ = false;
    } else if (a == "style") {
        if (pattern::find(arg)) {
            cfg_.style = arg;
            e_.set_style(arg);
            note("Beat: " + pattern::find(arg)->name);
        }
    } else if (a == "song") apply_song((int)num);
    else if (a == "next_song") {
        int n = (int)set_songs().size();
        apply_song(cfg_.song + 1 < n ? cfg_.song + 1 : cfg_.song);
    } else if (a == "play_song") {   // a song by id, from anywhere
        songs::Song s;
        if (lib_.get(arg, s)) {
            auto list = set_songs();
            cfg_.song = -1;
            for (size_t i = 0; i < list.size(); i++)
                if (list[i].id == arg) cfg_.song = (int)i;
            apply(s);
        }
    } else if (a == "setlist") {
        songs::SetList l;
        cfg_.setlist = arg.empty() || lib_.get_setlist(arg, l) ? arg : cfg_.setlist;
        cfg_.song = -1;
        note(cfg_.setlist.empty() ? "No set list: every song, A to Z" : "Set list: " + cfg_.setlist);
    }
    else if (a == "prev_song") apply_song(cfg_.song > 0 ? cfg_.song - 1 : 0);
    else if (a == "volume") {
        cfg_.volume = std::max(0.0, std::min(1.5, num));
        push_settings();
    } else if (a == "click" || a == "click_toggle") {
        cfg_.click = !cfg_.click;
        push_settings();
        note(cfg_.click ? "Click on" : "Click off");
    } else if (a == "hit") e_.hit((int)num, 110);
    else if (a == "panic") {
        e_.panic();
        note("Everything stopped (panic).");
    } else if (a == "quit") quit = true;
    else return false;
    config::save(cfg_);
    return true;
}

static std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c < 0x20) continue;
        o += c;
    }
    return o;
}

std::string Control::lists_json() {
    std::lock_guard<std::recursive_mutex> guard(act_mu_);
    std::ostringstream o;
    o << "{\"favorites\":[";
    for (size_t i = 0; i < cfg_.favorites.size(); i++) o << (i ? "," : "") << "\"" << esc(cfg_.favorites[i]) << "\"";
    o << "],\"styles\":[";
    bool first = true;
    for (const auto *s : pattern::styles()) {
        o << (first ? "" : ",") << "[\"" << s->id << "\",\"" << esc(s->name) << "\",\"" << esc(s->family) << "\",\"" << esc(s->meter) << "\","
          << s->bpm << ",\"" << esc(s->about) << "\"]";
        first = false;
    }
    o << "],\"kits\":[";
    first = true;
    for (const auto &k : kit::list(paths::kits())) {
        o << (first ? "" : ",") << "[\"" << esc(k.slug) << "\",\"" << esc(k.name) << "\",\"" << esc(k.style) << "\"]";
        first = false;
    }
    o << "],\"keys\":{";
    first = true;
    for (const auto &kv : cfg_.keys) {
        o << (first ? "" : ",") << "\"" << esc(kv.second) << "\":\"" << esc(kv.first) << "\"";
        first = false;
    }
    o << "}}";
    return o.str();
}

std::string Control::json() {
    std::lock_guard<std::recursive_mutex> guard(act_mu_);
    engine::Status st = e_.status();
    std::ostringstream o;
    o << "{\"playing\":" << (st.playing ? "true" : "false") << ",\"counting\":" << (st.counting ? "true" : "false")
      << ",\"bar\":" << st.bar << ",\"beat\":" << st.beat << ",\"beats\":" << st.beats << ",\"bpm\":" << std::lround(st.bpm) << ",\"bpmf\":" << st.bpm
      << ",\"section\":\"" << pattern::section_name(st.section) << "\",\"next\":\"" << (st.next_section >= 0 ? pattern::section_name(st.next_section) : "")
      << "\",\"ending\":" << (st.ending ? "true" : "false") << ",\"clapping\":" << (st.clapping ? "true" : "false")
      << ",\"clap_now\":" << (st.clap_now ? "true" : "false") << ",\"pause\":" << st.pause << ",\"style\":\"" << esc(cfg_.style) << "\",\"style_name\":\"" << esc(st.style)
      << "\",\"kit\":\"" << esc(cfg_.kit) << "\",\"kit_name\":\"" << esc(st.kit) << "\",\"loop\":\"" << esc(st.loop) << "\",\"pedal\":\""
      << esc(st.pedal_state) << "\",\"bars\":" << bars_for(track) << ",\"bars_a\":" << cfg_.loop_bars << ",\"bars_b\":" << bars_for(1) << ",\"track\":\"" << (track ? "B" : "A") << "\",\"click\":"
      << (cfg_.click ? "true" : "false") << ",\"volume\":" << cfg_.volume << ",\"song\":" << cfg_.song << ",\"song_id\":\"" << esc(current_song)
      << "\",\"setlist\":\"" << esc(cfg_.setlist) << "\",\"problem\":\"" << esc(st.problem) << "\",\"grid\":" << (st.grid ? "true" : "false")
      << ",\"preview\":" << (preview ? "true" : "false") << ",\"swing\":" << cfg_.swing << ",\"pocket\":" << cfg_.pocket_ms
      << ",\"phase\":" << st.phase << ",\"connection\":\"" << esc(connection(setup(), e_.healthy())) << "\",\"pedal_ok\":"
      << (setup().looper && e_.healthy() ? "true" : "false") << ",\"loose\":" << cfg_.humanize << ",\"locked\":" << (cfg_.locked ? "true" : "false") << ",\"pedal_mode\":\"" << esc(cfg_.pedal_mode) << "\""
      << ",\"network\":\"" << esc(cfg_.network) << "\",\"network_name\":\"" << esc(network().label()) << "\",\"network_ip\":\"" << esc(network().ip)
      << "\",\"network_only\":" << (cfg_.network_only ? "true" : "false")
      << ",\"has_a\":" << (st.has_a ? "true" : "false") << ",\"has_b\":" << (st.has_b ? "true" : "false") << ",\"empty_a\":" << (st.empty_a ? "true" : "false") << ",\"empty_b\":" << (st.empty_b ? "true" : "false") << ",\"playing_track\":\""
      << (st.playing_track == 0 ? "A" : st.playing_track == 1 ? "B" : "") << "\",\"next_track\":\"" << (st.next_track == 0 ? "A" : st.next_track == 1 ? "B" : st.next_track == 2 ? "stop" : "") << "\",\"style_swing\":" << (pattern::find(cfg_.style) ? pattern::find(cfg_.style)->swing : 0) << ",\"songs\":[";
    {
        auto list = set_songs();
        for (size_t i = 0; i < list.size(); i++) o << (i ? "," : "") << "[\"" << esc(list[i].id) << "\",\"" << esc(list[i].name) << "\"]";
    }
    o << "],\"log\":[";
    {
        std::lock_guard<std::mutex> lock(mu_);
        size_t from = recent.size() > 6 ? recent.size() - 6 : 0;
        for (size_t i = from; i < recent.size(); i++) o << (i > from ? "," : "") << "\"" << esc(recent[i]) << "\"";
    }
    o << "]}";
    return o.str();
}

// ----------------------------------------------------------------------------- the screen

namespace {

// ---- the looks: a theme (its colors, panels, keycaps, wordmark) and a style of beat lights, picked
// separately (y on the main screen). Colors are xterm-256 numbers; the Linux console gets the nearest 16.
enum Cards { STRIPE, FLAT, FRAME };
struct Theme {
    const char *id, *name, *about;
    int bg, panel, raised, line, fg, dim, faint, hi, accent, on_acc, head, good, bad, a_col, b_col, rec, sel, popup;
    int beat1, beat, beat1_fade, beat_fade, count, count_fade, past, sweep;
    int chip_bg, chip_fg, warn_bg, prob_bg, prob_fg;
    std::vector<int> mark;   // the wordmark, letter by letter
    Cards cards;
    bool big_tempo, bracket_keys;
    const char *lights;      // the beat lights it comes with
};
const std::vector<Theme> &themes() {
    static const std::vector<Theme> T = {
        {"stage", "Stage", "near-black stage, soft panels, a warm accent; A blue, B violet",
         234, 235, 237, 239, 252, 245, 241, 231, 214, 16, 117, 114, 203, 39, 141, 197, 61, 236,
         205, 87, 168, 80, 214, 172, 60, 117, 239, 231, 124, 52, 217, {214, 209, 204, 205, 170, 135, 99}, STRIPE, true, false, "blocks"},
        {"classic", "Classic", "the first look: plain text on black, amber and cyan, dot lights",
         234, 234, 236, 238, 252, 244, 242, 231, 214, 16, 81, 114, 203, 75, 141, 203, 24, 236,
         114, 114, 114, 114, 214, 214, 242, 81, 234, 214, 160, 52, 217, {214, 214, 214, 214, 214, 214, 214}, FLAT, false, true, "dots"},
        {"neon", "Neon Grid", "retro-futuristic: pink and cyan on midnight, framed panels, an LED meter",
         16, 17, 18, 54, 189, 147, 97, 231, 213, 16, 51, 49, 197, 51, 213, 197, 55, 17,
         213, 51, 162, 37, 220, 172, 54, 51, 54, 231, 125, 53, 218, {201, 207, 213, 177, 141, 105, 51}, FRAME, true, false, "leds"},
        {"phosphor", "Green Screen", "a green phosphor terminal: every color is green",
         16, 16, 22, 22, 83, 35, 28, 157, 118, 16, 46, 118, 157, 46, 120, 118, 22, 16,
         118, 46, 70, 34, 157, 71, 22, 46, 22, 157, 22, 22, 157, {46, 46, 46, 46, 46, 46, 46}, FRAME, true, true, "pips"},
        {"amber", "Amber", "an amber monochrome monitor, warm and easy on the eyes",
         16, 16, 52, 94, 214, 172, 130, 230, 220, 16, 214, 220, 230, 214, 222, 220, 94, 16,
         220, 214, 178, 172, 230, 179, 94, 214, 94, 230, 94, 52, 230, {214, 214, 214, 214, 214, 214, 214}, FRAME, true, true, "numbers"},
        {"daylight", "Daylight", "light and high-contrast, for bright rooms and outdoor stages",
         255, 254, 252, 250, 235, 240, 244, 232, 166, 231, 25, 28, 160, 25, 91, 160, 153, 255,
         166, 31, 173, 74, 166, 173, 250, 25, 238, 255, 160, 224, 88, {166, 167, 168, 133, 97, 61, 25}, STRIPE, true, false, "blocks"},
        {"ice", "Midnight Ice", "cool blues and silver, quiet on a dark stage",
         233, 234, 236, 238, 253, 249, 243, 231, 45, 16, 153, 115, 210, 45, 147, 204, 24, 235,
         123, 81, 74, 67, 45, 38, 24, 153, 238, 231, 89, 52, 217, {153, 117, 81, 45, 39, 33, 27}, STRIPE, true, false, "sweep"},
    };
    return T;
}
// the beat lights: (id, name, what they look like)
const std::vector<std::vector<std::string>> &light_styles() {
    static const std::vector<std::vector<std::string>> L = {
        {"blocks", "Blocks", "a big block for each beat, the bar sweeping under them"},
        {"dots", "Dots", "a dot for each beat, like the first Barline"},
        {"leds", "LED meter", "segments that fill through each beat, like a VU meter"},
        {"numbers", "Big numbers", "the beat you're on, big enough to read from across the stage"},
        {"pips", "Diamonds", "a diamond for each beat"},
        {"sweep", "Sweep", "one bar that sweeps across the measure, beats marked under it"}};
    return L;
}

// the colors in use (set from the theme)
int BG, PANEL, RAISED, LINE, FG, DIM, FAINT, HI, ACCENT, ON_ACC, HEAD, GOOD, BAD, A_COL, B_COL, REC, SEL, POPUP;
int BEAT1, BEAT, BEAT1_FADE, BEAT_FADE, COUNT, COUNT_FADE, PAST, SWEEP, CHIP_BG, CHIP_FG, WARN_BG, PROB_BG, PROB_FG;
const Theme *g_theme = nullptr;
std::string g_lights = "blocks";

void apply_look(const std::string &theme, const std::string &lights) {
    const Theme *t = &themes()[0];
    for (const auto &x : themes())
        if (theme == x.id) t = &x;
    g_theme = t;
    BG = t->bg, PANEL = t->panel, RAISED = t->raised, LINE = t->line, FG = t->fg, DIM = t->dim, FAINT = t->faint, HI = t->hi;
    ACCENT = t->accent, ON_ACC = t->on_acc, HEAD = t->head, GOOD = t->good, BAD = t->bad, A_COL = t->a_col, B_COL = t->b_col;
    REC = t->rec, SEL = t->sel, POPUP = t->popup, BEAT1 = t->beat1, BEAT = t->beat, BEAT1_FADE = t->beat1_fade, BEAT_FADE = t->beat_fade;
    COUNT = t->count, COUNT_FADE = t->count_fade, PAST = t->past, SWEEP = t->sweep, CHIP_BG = t->chip_bg, CHIP_FG = t->chip_fg;
    WARN_BG = t->warn_bg, PROB_BG = t->prob_bg, PROB_FG = t->prob_fg;
    g_lights = t->lights;
    for (const auto &l : light_styles())
        if (lights == l[0]) g_lights = lights;
}

// what a click on the screen does (a keycap, a loop tile): rebuilt with every frame
struct Hot {
    int x, y, w;
    std::string action, arg;
};
std::vector<Hot> g_hots;

int text_w(const std::string &t) {
    int n = 0;
    for (unsigned char c : t) n += (c & 0xC0) != 0x80;
    return n;
}

std::string upper(std::string t) {
    for (auto &c : t) c = (char)std::toupper((unsigned char)c);
    return t;
}

// a key as it's printed on a keycap
std::string cap(const std::string &key) {
    if (key == "space") return "Space";
    if (key == "pgdn") return "PgDn";
    if (key == "pgup") return "PgUp";
    if (key == "enter") return "Enter";
    if (key == "esc") return "Esc";
    if (key == "tab") return "Tab";
    if (term::ascii()) return key == "left" ? "<-" : key == "right" ? "->" : key == "up" ? "Up" : key == "down" ? "Dn" : key;
    return key == "left" ? "\xE2\x86\x90" : key == "right" ? "\xE2\x86\x92" : key == "up" ? "\xE2\x86\x91" : key == "down" ? "\xE2\x86\x93" : key;
}

// keycaps for one key or a pair ("[ ]"), each in its own cap; returns how wide (draw = false: just measure)
int caps(int x, int y, const std::string &keys, bool draw = true) {
    int w = 0;
    std::istringstream in(keys);
    std::string one;
    while (in >> one) {
        std::string c = g_theme->bracket_keys ? "[" + cap(one) + "]" : " " + cap(one) + " ";
        if (draw) term::put(x + w, y, c, g_theme->bracket_keys ? ACCENT : CHIP_FG, g_theme->bracket_keys ? -2 : CHIP_BG, term::A_BOLD);
        w += text_w(c) + 1;
    }
    return std::max(0, w - 1);
}

// a keycap and what it does; returns where the next one goes (or x unchanged if it doesn't fit before max_x)
int chip(int x, int y, const std::string &key, const std::string &label, int max_x, const std::string &action = "", int bg = BG) {
    int kw = caps(0, 0, key, false);
    int w = kw + 1 + text_w(label);
    if (x + w > max_x) return x;
    caps(x, y, key);
    term::put(x + kw + 1, y, label, DIM, bg);
    if (!action.empty()) g_hots.push_back({x, y, w, action, ""});
    return x + w + 2;
}

// a panel: a tinted card with a colored edge and a small title
void card(int x, int y, int w, int h, const std::string &title, int accent) {
    term::fill(x, y, w, h, PANEL);
    bool a = term::ascii();
    if (g_theme->cards == STRIPE)
        for (int i = 0; i < h; i++) term::put(x, y + i, a ? "|" : "\xE2\x96\x8E", accent, PANEL);
    else if (g_theme->cards == FRAME) {   // a thin frame in the theme's line color, the title set into it
        for (int i = 1; i < w - 1; i++) {
            term::put(x + i, y, a ? "-" : "\xE2\x94\x80", LINE, PANEL);
            term::put(x + i, y + h - 1, a ? "-" : "\xE2\x94\x80", LINE, PANEL);
        }
        for (int i = 1; i < h - 1; i++) {
            term::put(x, y + i, a ? "|" : "\xE2\x94\x82", LINE, PANEL);
            term::put(x + w - 1, y + i, a ? "|" : "\xE2\x94\x82", LINE, PANEL);
        }
        term::put(x, y, a ? "+" : "\xE2\x94\x8C", LINE, PANEL);
        term::put(x + w - 1, y, a ? "+" : "\xE2\x94\x90", LINE, PANEL);
        term::put(x, y + h - 1, a ? "+" : "\xE2\x94\x94", LINE, PANEL);
        term::put(x + w - 1, y + h - 1, a ? "+" : "\xE2\x94\x98", LINE, PANEL);
        if (!title.empty()) term::put(x + 2, y, " " + upper(title) + " ", accent, PANEL, term::A_BOLD);
        return;
    }
    if (!title.empty()) term::put(x + 2, y, upper(title), accent, PANEL, term::A_BOLD);
}

// a pop-up (the pickers, the help): a raised card with a thin rounded outline
void box(int x, int y, int w, int h, const std::string &title) {
    term::fill(x, y, w, h, POPUP);
    bool a = term::ascii();
    for (int i = 1; i < w - 1; i++) {
        term::put(x + i, y, a ? "-" : "\xE2\x94\x80", LINE, POPUP);
        term::put(x + i, y + h - 1, a ? "-" : "\xE2\x94\x80", LINE, POPUP);
    }
    for (int i = 1; i < h - 1; i++) {
        term::put(x, y + i, a ? "|" : "\xE2\x94\x82", LINE, POPUP);
        term::put(x + w - 1, y + i, a ? "|" : "\xE2\x94\x82", LINE, POPUP);
    }
    term::put(x, y, a ? "+" : "\xE2\x95\xAD", LINE, POPUP);
    term::put(x + w - 1, y, a ? "+" : "\xE2\x95\xAE", LINE, POPUP);
    term::put(x, y + h - 1, a ? "+" : "\xE2\x95\xB0", LINE, POPUP);
    term::put(x + w - 1, y + h - 1, a ? "+" : "\xE2\x95\xAF", LINE, POPUP);
    term::put(x + 2, y, " " + upper(title) + " ", HEAD, POPUP, term::A_BOLD);
}

// the wordmark, in a sunset fade
void wordmark(int x, int y) {
    const char *w = "BARLINE";
    for (int i = 0; i < 7; i++) term::put(x + i, y, std::string(1, w[i]), g_theme->mark[(size_t)i % g_theme->mark.size()], BG, term::A_BOLD);
}

// big digits for the tempo: 3x5 pixels, each pixel two cells wide, drawn with half blocks (3 rows tall)
void big_number(int x, int y, int n, int fg) {
    static const char *D[10][5] = {
        {"###", "#.#", "#.#", "#.#", "###"}, {".#.", "##.", ".#.", ".#.", "###"}, {"###", "..#", "###", "#..", "###"},
        {"###", "..#", ".##", "..#", "###"}, {"#.#", "#.#", "###", "..#", "..#"}, {"###", "#..", "###", "..#", "###"},
        {"###", "#..", "###", "#.#", "###"}, {"###", "..#", "..#", ".#.", ".#."}, {"###", "#.#", "###", "#.#", "###"},
        {"###", "#.#", "###", "..#", "###"}};
    std::string t = std::to_string(n);
    for (char c : t) {
        const char **g = D[c - '0'];
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) {
                bool top = g[row * 2][col] == '#', bot = row * 2 + 1 < 5 && g[row * 2 + 1][col] == '#';
                std::string ch = term::ascii() ? (top || bot ? "#" : " ")
                               : top && bot ? "\xE2\x96\x88" : top ? "\xE2\x96\x80" : bot ? "\xE2\x96\x84" : " ";
                term::put(x + col * 2, y + row, ch + ch, fg, PANEL);
            }
        x += 8;
    }
}

// the beat lights, in the style picked. Inside a card from row y+1 to y+ch-2.
void beat_lights(const engine::Status &s, int gx, int y, int gw, int ch, bool roomy) {
    bool a = term::ascii();
    int beats = std::max(1, s.beats), inner = gw - 6, x0 = gx + 3;
    auto lit = [&](int q) {   // the color of beat q (1..beats)
        bool now = s.grid && q == s.beat;
        if (!s.grid) return RAISED;
        if (now) {
            bool fade = s.phase > 0.6;   // the lit beat fades through its length
            return s.counting ? (fade ? COUNT_FADE : COUNT) : q == 1 ? (fade ? BEAT1_FADE : BEAT1) : (fade ? BEAT_FADE : BEAT);
        }
        return q < s.beat ? PAST : RAISED;
    };
    auto label = [&](int x, int row, int q) {
        bool now = s.grid && q == s.beat;
        term::put(x, row, std::to_string(q), now ? HI : FAINT, PANEL, now ? term::A_BOLD : 0);
    };
    auto rep = [](const char *g, int n) {
        std::string o;
        for (int i = 0; i < n; i++) o += g;
        return o;
    };
    // the bar sweeping across the measure, in eighths of a cell
    auto sweep = [&](int x, int row, int w) {
        double done = s.grid ? std::min(1.0, ((double)std::max(0, s.beat - 1) + s.phase) / beats) : 0;
        int eighths = (int)(done * w * 8);
        for (int c = 0; c < w; c++) {
            int e = std::max(0, std::min(8, eighths - c * 8));
            std::string g = e >= 8 ? term::g_block() : e > 0 ? term::g_partial(e) : (a ? "." : "\xE2\x96\x81");
            term::put(x + c, row, g, e ? SWEEP : RAISED, PANEL);
        }
    };
    int top = y + 1 + (roomy ? 1 : 0), bottom = y + ch - 2;
    std::string style = g_lights;
    if (style == "numbers") {
        if (roomy) {
            int q = s.grid ? s.beat : 1;
            big_number(x0, y + 1, q, s.grid ? lit(q) : FAINT);
            int tx = x0 + 9;
            term::put(tx, y + 2, "of " + std::to_string(beats), DIM, PANEL, term::A_BOLD);
            for (int k = 1; k <= beats && tx + 2 * k < gx + gw - 2; k++) term::put(tx + 2 * (k - 1), y + 4, a ? (k == s.beat && s.grid ? "*" : "o") : (k == s.beat && s.grid ? "\xE2\x97\x8F" : "\xE2\x97\x8B"), lit(k), PANEL);
            sweep(tx + 2 * beats + 2, y + 4, std::max(4, gx + gw - 3 - (tx + 2 * beats + 2)));
        } else {
            term::put(x0, y + 1, s.grid ? "BEAT " + std::to_string(s.beat) + " / " + std::to_string(beats) : "BEAT - / " + std::to_string(beats), s.grid ? lit(s.beat) : FAINT, PANEL, term::A_BOLD);
            sweep(x0, y + 2, std::min(inner, 24));
        }
        return;
    }
    if (style == "sweep") {
        int w = std::max(8, inner);
        for (int r = 0; r < (roomy ? 2 : 1); r++) sweep(x0, top + r, w);
        if (roomy)
            for (int q = 1; q <= beats; q++) {
                int bx = x0 + (int)((double)(q - 1) * w / beats);
                term::put(bx, y + 4, a ? "|" : "\xE2\x94\x82", lit(q), PANEL);
                label(bx + 1, y + 4, q);
            }
        return;
    }
    if (style == "dots" || style == "pips") {
        bool dots = style == "dots";
        int step = std::max(3, std::min(8, inner / beats));
        for (int q = 1; q <= beats; q++) {
            int bx = x0 + (q - 1) * step;
            bool on = s.grid && q <= s.beat;
            std::string g = a ? (on ? "*" : "o") : dots ? (on ? "\xE2\x97\x8F" : "\xE2\x97\x8B") : (on ? "\xE2\x97\x86" : "\xE2\x97\x87");
            term::put(bx, top, g, lit(q), PANEL, term::A_BOLD);
            if (roomy) label(bx, y + 4, q);
        }
        if (roomy || ch > 3) sweep(x0, roomy ? bottom : y + 2, std::max(4, beats * step - (step - 1)));
        return;
    }
    int bw = std::max(1, std::min(14, inner / beats - 1));
    if (style == "leds") {   // segments filling through each beat
        int segs = std::max(2, bw / 2);
        for (int q = 1; q <= beats; q++) {
            int bx = x0 + (q - 1) * (bw + 1);
            int on = !s.grid ? 0 : q < s.beat ? segs : q == s.beat ? std::max(1, (int)std::ceil(s.phase * segs)) : 0;
            for (int g = 0; g < segs; g++) {
                int c = g < on ? (q == s.beat ? lit(q) : PAST) : RAISED;
                for (int r = 0; r < (roomy ? 2 : 1); r++) term::put(bx + g * 2, top + r, a ? "#" : "\xE2\x96\x8C", c, PANEL);
            }
            if (roomy) label(bx, y + 4, q);
        }
        return;
    }
    // blocks
    for (int q = 1; q <= beats; q++) {
        int bx = x0 + (q - 1) * (bw + 1);
        bool now = s.grid && q == s.beat;
        for (int r = 0; r < (roomy ? 2 : 1); r++) term::put(bx, top + r, a ? std::string((size_t)bw, now ? '#' : '=') : rep("\xE2\x96\x88", bw), lit(q), PANEL);
        if (roomy) label(bx, y + 4, q);
    }
    if (roomy || ch > 3) sweep(x0, roomy ? bottom : y + 2, beats * (bw + 1) - 1);
}

std::string key_name(const term::Event &ev) {
    switch (ev.key) {
    case ' ': return "space";
    case term::K_PGDN: return "pgdn";
    case term::K_PGUP: return "pgup";
    case term::K_LEFT: return "left";
    case term::K_RIGHT: return "right";
    case term::K_UP: return "up";
    case term::K_DOWN: return "down";
    case term::K_ENTER: return "enter";
    case term::K_TAB: return "tab";
    case term::K_ESC: return "esc";
    }
    if (ev.key > 32 && ev.key < 127) return std::string(1, (char)std::tolower(ev.key));
    return "";
}

struct Picker {
    std::string title;
    std::vector<std::pair<std::string, std::string>> items;   // (value, label)
    int sel = 0;
    std::string action;
    std::string kind;   // "beats" / "fav" / "kits" / "songs"
};


bool is_fav(const config::Config &c, const std::string &id) { return std::find(c.favorites.begin(), c.favorites.end(), id) != c.favorites.end(); }

std::string beat_label(const config::Config &c, const pattern::Style *s) {
    return std::string(is_fav(c, s->id) ? "\xE2\x98\x85 " : "  ") + s->family + ": " + s->name + "  " + s->meter + ", " + std::to_string(s->bpm) +
           " BPM";
}

// the Look picker: every theme, then every style of beat lights (moving through it shows each one)
Picker *look_picker(const config::Config &cfg) {
    Picker *p = new Picker{"Look  (Enter keeps it, Esc goes back)", {}, 0, "look", "look"};
    const char *tick = term::ascii() ? "* " : "\xE2\x9C\x93 ";
    for (const auto &t : themes()) {
        bool on = cfg.tui_theme == t.id;
        if (on) p->sel = (int)p->items.size();
        p->items.push_back({std::string("theme:") + t.id, std::string(on ? tick : "  ") + "Theme   " + t.name + "  \xC2\xB7  " + t.about});
    }
    p->items.push_back({"lights:theme", std::string(cfg.tui_lights == "theme" ? tick : "  ") + "Lights  The theme's own"});
    for (const auto &l : light_styles())
        p->items.push_back({"lights:" + l[0], std::string(cfg.tui_lights == l[0] ? tick : "  ") + "Lights  " + l[1] + "  \xC2\xB7  " + l[2]});
    return p;
}

Picker *beats_picker(const config::Config &cfg, bool favorites_only) {
    Picker *p = new Picker{favorites_only ? "Favorite Beats" : "All Beats", {}, 0, "style", favorites_only ? "fav" : "beats"};
    if (favorites_only) {
        for (const auto &id : cfg.favorites)
            if (const pattern::Style *s = pattern::find(id)) {
                if (s->id == cfg.style) p->sel = (int)p->items.size();
                p->items.push_back({s->id, "\xE2\x98\x85 " + s->name + "  " + s->meter + ", " + std::to_string(s->bpm) + " BPM"});
            }
        p->items.push_back({"*all", "   All beats..."});
    } else
        for (const auto *s : pattern::styles()) {
            if (s->id == cfg.style) p->sel = (int)p->items.size();
            p->items.push_back({s->id, beat_label(cfg, s)});
        }
    return p;
}

// ---- the beat builder
const std::vector<std::pair<std::string, std::pair<int, int>>> METERS = {
    {"4/4", {4, 16}}, {"4/4 shuffle", {4, 12}}, {"3/4", {3, 12}}, {"6/8", {3, 12}}, {"5/4", {5, 20}}, {"7/4", {7, 28}}};

struct Builder {
    pattern::Style work;
    int section = pattern::VERSE;   // the part being edited (any of them)
    bool split_fills = false;       // the fill back to the verse is its own (else it follows the fill to the chorus)
    int row = 0, col = 0, top = 0;
    bool dirty = false, naming = false, confirm_leave = false;
    std::string name_buf;
    std::string back_style;   // the beat that was playing before
    bool preview_on = false;  // the builder turned "hear on laptop" on (and turns it off on leaving)
    bool started = false;     // ...and started the drums (stopped on leaving)
    std::string saved_id;     // what it was saved as (for the song being edited)
};

void set_hit(pattern::Part &p, int step, int note, int vel) {
    p.hits.erase(std::remove_if(p.hits.begin(), p.hits.end(), [&](const pattern::Hit &h) { return h.step == step && h.note == note; }), p.hits.end());
    if (vel > 0) p.hits.push_back({step, note, vel});
}

int hit_at(const pattern::Part &p, int step, int note) {
    for (const auto &h : p.hits)
        if (h.step == step && h.note == note) return h.vel;
    return 0;
}

// ---- the song editor (the laptop's; the iPad has its own)
enum SongField { F_NAME, F_ARTIST, F_KEY, F_TEMPO, F_BEAT, F_KIT, F_BARS, F_COUNT, F_SLOT, F_SWING, F_POCKET, F_LOOSE, F_GRID, F_NOTES, F_LYRICS, F_COUNT_ };

struct SongForm {
    songs::Song s;
    int row = 0;
    bool typing = false, dirty = false, confirm_leave = false;
    std::string buf;
};

bool text_field(int f) { return f == F_NAME || f == F_ARTIST || f == F_KEY || f == F_NOTES || f == F_TEMPO; }

std::string *text_of(songs::Song &s, int f) {
    switch (f) {
    case F_NAME: return &s.name;
    case F_ARTIST: return &s.artist;
    case F_KEY: return &s.key;
    case F_NOTES: return &s.notes;
    }
    return nullptr;
}

std::string pct(double v) { return std::to_string((int)std::lround(v * 100)) + "%"; }

std::string field_label(int f) {
    static const char *L[] = {"Name", "Artist", "Key", "Tempo", "Beat", "Kit", "Loop length", "Count-in", "Pedal slot", "Swing", "Pocket", "Looseness", "Grid", "Notes", "Lyrics"};
    return L[f];
}

std::string field_value(const songs::Song &s, int f) {
    char b[64];
    switch (f) {
    case F_NAME: return s.name;
    case F_ARTIST: return s.artist;
    case F_KEY: return s.key;
    case F_TEMPO: std::snprintf(b, sizeof b, "%g BPM", s.bpm); return b;
    case F_BEAT: {
        const pattern::Style *st = pattern::find(s.style);
        return st ? st->name + "  (" + st->meter + ")" : s.style + " (missing)";
    }
    case F_KIT: {
        if (s.kit.empty()) return "(keep the kit that's on)";
        for (const auto &k : kit::list(paths::kits()))
            if (k.slug == s.kit) return k.name;
        return s.kit;
    }
    case F_BARS: return std::to_string(s.loop_bars) + (s.loop_bars == 1 ? " bar" : " bars");
    case F_COUNT: return s.count_in == 0 ? "none" : std::to_string(s.count_in) + (s.count_in == 1 ? " bar" : " bars");
    case F_SLOT: return s.slot < 0 ? "none" : std::to_string(s.slot);
    case F_SWING: return s.swing < 0 ? "the beat's own" : pct(s.swing);
    case F_POCKET: {
        int p = (int)std::lround(s.pocket);
        return p == 0 ? "on the beat" : p > 0 ? "laid back " + std::to_string(p) + " ms" : "pushed " + std::to_string(-p) + " ms";
    }
    case F_LOOSE: return s.loose < 0 ? "as set" : pct(s.loose);
    case F_GRID: return s.locked < 0 ? "as set" : s.locked ? "Locked" : "Human";
    case F_NOTES: return s.notes;
    case F_LYRICS: {
        int lines = s.lyrics.empty() ? 0 : 1 + (int)std::count(s.lyrics.begin(), s.lyrics.end(), '\n');
        return lines ? std::to_string(lines) + " lines" : "(none)";
    }
    }
    return "";
}

// left/right on a field: the next value down or up
void step_field(songs::Song &s, int f, int d) {
    auto steps = [&](double v, std::initializer_list<double> list) {
        std::vector<double> l(list);
        size_t i = 0;
        for (size_t j = 0; j < l.size(); j++)
            if (std::fabs(l[j] - v) < std::fabs(l[i] - v)) i = j;
        int k = std::max(0, std::min((int)l.size() - 1, (int)i + d));
        return l[(size_t)k];
    };
    switch (f) {
    case F_TEMPO: s.bpm = std::max(30.0, std::min(300.0, std::round(s.bpm) + d)); break;
    case F_BARS: s.loop_bars = (int)steps(s.loop_bars, {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64}); break;
    case F_COUNT: s.count_in = std::max(0, std::min(2, s.count_in + d)); break;
    case F_SLOT: s.slot = std::max(-1, std::min(127, s.slot + d)); break;
    case F_SWING: s.swing = steps(s.swing, {-1, 0, 0.1, 0.2, 0.25, 0.3, 0.35, 0.4, 0.5, 0.6, 0.75, 1}); break;
    case F_POCKET: s.pocket = std::max(-15.0, std::min(15.0, std::round(s.pocket) + d)); break;
    case F_LOOSE: s.loose = steps(s.loose, {-1, 0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1}); break;
    case F_GRID: s.locked = std::max(-1, std::min(1, s.locked + d)); break;
    }
}

// the lyrics in your text editor ($VISUAL, $EDITOR, else nano, else vi); false if it couldn't run
bool edit_in_editor(std::string &text) {
    std::string path = paths::data() + "/.lyrics-edit.txt";
    std::string t = text.empty() || text.back() == '\n' ? text : text + "\n";
    if (!sys::write_private(path, t)) return false;
    term::stop();
    bool ok = sys::edit_text_file(path);
    term::start();
    term::invalidate();
    if (ok) {
        std::ifstream f(path);
        std::ostringstream o;
        o << f.rdbuf();
        text = o.str();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    }
    std::remove(path.c_str());
    return ok;
}

}  // namespace

int run(Control &ctl) {
    if (!term::start()) {
        std::fprintf(stderr, "Barline needs a terminal.\n");
        return 1;
    }
    // plain where the terminal is plain: NO_COLOR, the Linux text console (16 colors), no UTF-8
    auto env = [](const char *n) { const char *v = getenv(n); return std::string(v ? v : ""); };
    std::string term_name = env("TERM"), locale = env("LC_ALL").empty() ? (env("LC_CTYPE").empty() ? env("LANG") : env("LC_CTYPE")) : env("LC_ALL");
    if (!env("NO_COLOR").empty() || term_name == "dumb") term::set_color_mode(term::COLOR_NONE);
    else if (term_name == "linux" || term_name.rfind("vt", 0) == 0) term::set_color_mode(term::COLOR_16);
    for (auto &c : locale) c = (char)std::tolower((unsigned char)c);
    if (locale.find("utf-8") == std::string::npos && locale.find("utf8") == std::string::npos) term::set_ascii(true);
    config::Config &cfg = ctl.cfg();
    std::string try_theme, try_lights;   // the Look picker's preview (empty: what's saved)
    apply_look(cfg.tui_theme, cfg.tui_lights);
    std::unique_ptr<Picker> pick;
    std::unique_ptr<Builder> build;
    std::unique_ptr<SongForm> form;
    bool help = false;
    std::string beats_dir = paths::data() + "/beats";
    auto k = [&](const char *a) { return cfg.keys.count(a) ? cfg.keys.at(a) : std::string("?"); };
    auto open_builder = [&](const pattern::Style *from) {
        build.reset(new Builder());
        build->work = from ? *from : *pattern::find("rock");
        build->back_style = cfg.style;
        if (!build->work.user) {   // a built-in beat: build on a copy of it
            build->work.id.clear();
            build->work.name = "My " + build->work.name;
            build->work.about.clear();
            build->dirty = true;
        }
        if (build->work.parts[pattern::CHORUS].hits.empty()) build->work.parts[pattern::CHORUS] = build->work.parts[pattern::VERSE];
        build->split_fills = !pattern::same_part(build->work.parts[pattern::FILL_VERSE], build->work.parts[pattern::FILL_CHORUS]);
        ctl.engine().audition(-1);
    };
    auto open_form = [&](const songs::Song *from) {
        form.reset(new SongForm());
        if (from) form->s = *from;
        else {   // a new song: starts from what's playing
            form->s.style = cfg.style;
            form->s.bpm = cfg.bpm;
            form->s.kit = cfg.kit;
            form->s.loop_bars = cfg.loop_bars;
            form->s.loop_bars_b = cfg.loop_bars_b;
            form->dirty = true;
        }
    };
    auto save_form = [&] {
        SongForm &f = *form;
        if (f.s.name.empty()) {
            ctl.note("The song needs a name (the first line).");
            f.row = F_NAME;
            return false;
        }
        bool fresh = f.s.id.empty();
        f.s.estimated = false;   // saved by you: checked
        f.s.confirmed = true;
        std::string err, id = ctl.library().save(f.s, &err);
        if (id.empty()) {
            ctl.note(err);
            return false;
        }
        f.s.id = id;
        if (fresh && !cfg.setlist.empty()) {   // a new song goes on the end of the set list in use
            songs::SetList l;
            if (ctl.library().get_setlist(cfg.setlist, l)) {
                l.songs.push_back(id);
                ctl.library().save_setlist(l);
            }
        }
        if (ctl.current_song == id) ctl.apply(f.s);
        f.dirty = f.confirm_leave = false;
        ctl.note("Saved \"" + f.s.name + "\"");
        return true;
    };
    auto preview = [&] {
        if (!build) return;
        pattern::preview(build->work);
        ctl.engine().set_style("edit-preview");
    };
    // what a key (or a click on its keycap) does on the main screen
    auto main_action = [&](const std::string &action) {
        if (action == "look") pick.reset(look_picker(cfg));
        else if (action == "kits") {
            pick.reset(new Picker{"Kits", {}, 0, "kit", "kits"});
            for (const auto &i : kit::list(paths::kits())) {
                if (i.slug == cfg.kit) pick->sel = (int)pick->items.size();
                pick->items.push_back({i.slug, i.name + "  (" + i.style + ")"});
            }
        } else if (action == "beats") pick.reset(beats_picker(cfg, !cfg.favorites.empty()));
        else if (action == "all_beats") pick.reset(beats_picker(cfg, false));
        else if (action == "builder") open_builder(pattern::find(cfg.style));
        else if (action == "songs") {
            auto list = ctl.set_songs();
            pick.reset(new Picker{cfg.setlist.empty() ? "Songs" : "Set List: " + cfg.setlist, {}, std::max(0, cfg.song), "song", "songs"});
            for (size_t i = 0; i < list.size(); i++)
                pick->items.push_back({std::to_string(i), std::to_string(i + 1) + ". " + list[i].name + (list[i].artist.empty() ? "" : "  - " + list[i].artist)});
            if (pick->items.empty()) {   // none yet: straight to a new one
                pick.reset();
                open_form(nullptr);
            }
        } else if (action == "help") help = true;
        else if (action == "window") {
            std::string url = ctl.window_url();
            if (url.empty()) ctl.note("The window needs the iPad page on (web = yes in the settings file).");
            else ctl.note(http::open_app_window(url) ? "Opened the Barline window." : "Couldn't find a web browser to open the window in.");
        } else if (!action.empty()) ctl.act(action);
    };
    while (!ctl.quit) {
        term::Event ev = term::read(40);
        if (ev.key == term::K_CLOSED) break;
        if (ev.key == term::K_MOUSE) {
            // the wheel scrolls a list; a click on a keycap or a loop tile does what it says
            if (ev.button == 64 || ev.button == 65) ev.key = ev.button == 64 ? term::K_UP : term::K_DOWN;
            else if (ev.button == 0 && !pick && !build && !form && !help) {
                for (const auto &h : g_hots)
                    if (ev.my == h.y && ev.mx >= h.x && ev.mx < h.x + h.w) {
                        if (!h.arg.empty()) ctl.act(h.action, h.arg);
                        else main_action(h.action);
                        break;
                    }
                ev.key = term::K_NONE;
            } else if (!help) ev.key = term::K_NONE;
        }
        if (ev.key != term::K_NONE && ev.key != term::K_RESIZE) {
            std::string key = key_name(ev);
            if (build && build->naming) {
                // typing the beat's name
                if (ev.key == term::K_ENTER) {
                    if (!build->name_buf.empty()) build->work.name = build->name_buf, build->dirty = true;
                    build->naming = false;
                } else if (ev.key == term::K_ESC) build->naming = false;
                else if (ev.key == term::K_BACKSPACE) {
                    if (!build->name_buf.empty()) build->name_buf.pop_back();
                } else if (ev.key >= 32 && ev.key < 127 && build->name_buf.size() < 40) build->name_buf += (char)ev.key;
            } else if (build) {
                Builder &b = *build;
                pattern::Style &w = b.work;
                pattern::Part &part = w.parts[b.section];
                auto lanes = pattern::drum_names();
                int cols = w.steps * std::max(1, part.bars);
                int note = lanes[(size_t)b.row].second;
                auto put = [&](int vel) {
                    set_hit(part, b.col, note, vel);
                    if (b.section == pattern::FILL_VERSE && !b.split_fills) w.parts[pattern::FILL_CHORUS] = part;
                    if (b.section == pattern::FILL_CHORUS) b.split_fills = true;
                    b.dirty = true;
                    b.confirm_leave = false;
                    preview();
                };
                if (key == "up") b.row = std::max(0, b.row - 1);
                else if (key == "down") b.row = std::min((int)lanes.size() - 1, b.row + 1);
                else if (key == "left") b.col = (b.col + cols - 1) % cols;
                else if (key == "right") b.col = (b.col + 1) % cols;
                else if (key == "space") put(hit_at(part, b.col, note) ? 0 : 100);
                else if (ev.key == 'x') put(100);
                else if (ev.key == 'X') put(124);
                else if (key == "o") put(72);
                else if (key == "g") put(42);
                else if (key == "." || ev.key == term::K_BACKSPACE || ev.key == term::K_DELETE) put(0);
                else if (key == "tab" || ev.key == term::K_BACKTAB) {
                    b.section = (b.section + (ev.key == term::K_BACKTAB ? pattern::SECTIONS - 1 : 1)) % pattern::SECTIONS;
                    b.col = std::min(b.col, w.steps * std::max(1, w.parts[b.section].bars) - 1);
                    ctl.engine().audition(b.section == pattern::VERSE ? -1 : b.section);
                } else if (key == "c" && b.section != pattern::VERSE) {
                    part = w.parts[pattern::VERSE];
                    put(hit_at(part, b.col, note));   // (saves the copy and refreshes)
                } else if (key == "l") {
                    part.bars = part.bars == 1 ? 2 : 1;
                    part.hits.erase(std::remove_if(part.hits.begin(), part.hits.end(), [&](const pattern::Hit &h) { return h.step >= w.steps * part.bars; }),
                                    part.hits.end());
                    b.col = std::min(b.col, w.steps * part.bars - 1);
                    b.dirty = true;
                    preview();
                } else if (key == "m") {
                    // the next time signature: the grid is redrawn at the new length (hits past it go)
                    size_t i = 0;
                    for (size_t j = 0; j < METERS.size(); j++)
                        if (METERS[j].first == w.meter) i = (j + 1) % METERS.size();
                    w.meter = METERS[i].first;
                    w.quarters = METERS[i].second.first;
                    int old_steps = w.steps;
                    w.steps = METERS[i].second.second;
                    for (auto &p : w.parts) {
                        std::vector<pattern::Hit> keep;
                        for (const auto &h : p.hits) {
                            int bar = h.step / old_steps, st = h.step % old_steps;
                            if (st < w.steps) keep.push_back({bar * w.steps + st, h.note, h.vel});
                        }
                        p.hits = keep;
                    }
                    // the count-in clicks every beat of the new meter, and the claps move to its backbeat
                    w.parts[pattern::CLAP] = pattern::default_clap(w.meter, w.quarters, w.steps);
                    w.parts[pattern::INTRO].hits.clear();
                    for (int q = 0; q < w.quarters; q++) w.parts[pattern::INTRO].hits.push_back({q * (w.steps / w.quarters), 42, q ? 96 : 120});
                    b.col = std::min(b.col, w.steps - 1);
                    b.dirty = true;
                    preview();
                } else if (key == "w") {
                    w.swing = w.swing < 0.2 ? 0.25 : w.swing < 0.45 ? 0.5 : 0;
                    b.dirty = true;
                    preview();
                } else if (ev.key == '+' || ev.key == '=' || ev.key == '-') {
                    w.bpm = std::max(30, std::min(300, w.bpm + (ev.key == '-' ? -1 : 1)));
                    cfg.bpm = w.bpm;
                    ctl.push_settings();
                    b.dirty = true;
                } else if (key == "n") {
                    b.naming = true;
                    b.name_buf = w.name;
                } else if (key == "enter") {
                    preview();
                    if (ctl.engine().status().playing) ctl.act("stop");
                    else {
                        if (!ctl.preview) {   // building a beat: you asked to hear it, so on the laptop too
                            ctl.act("preview", "on");
                            b.preview_on = true;
                        }
                        cfg.bpm = w.bpm;
                        ctl.push_settings();
                        ctl.act("start");
                        b.started = true;
                    }
                } else if (key == "s") {
                    std::string err;
                    if (const pattern::Style *saved = pattern::save_user(beats_dir, w, &err)) {
                        w = *saved;
                        b.dirty = false;
                        ctl.note("Saved \"" + w.name + "\" (in " + beats_dir + ")");
                        b.back_style = w.id;
                        b.saved_id = w.id;
                    } else
                        ctl.note("Couldn't save: " + err);
                } else if (key == "esc" || key == "q") {
                    if (b.dirty && !b.confirm_leave) {
                        b.confirm_leave = true;
                    } else {
                        if (b.started && ctl.engine().status().playing) ctl.act("stop");   // a preview doesn't keep going
                        if (b.preview_on) ctl.act("preview", "off");
                        ctl.engine().audition(-1);
                        ctl.act("style", b.back_style);
                        if (form && !b.saved_id.empty()) {   // came from a song: the song gets the saved beat
                            form->s.style = b.saved_id;
                            form->dirty = true;
                        }
                        build.reset();
                    }
                }
                if (build) b.top = std::max(0, std::min(b.top, b.row));
            } else if (form && !pick) {
                SongForm &f = *form;
                if (f.typing) {
                    if (ev.key == term::K_ENTER) {
                        if (f.row == F_TEMPO) {
                            double v = std::atof(f.buf.c_str());
                            if (v >= 30 && v <= 300) f.s.bpm = v;
                        } else
                            *text_of(f.s, f.row) = f.buf;
                        f.typing = false;
                        f.dirty = true;
                    } else if (ev.key == term::K_ESC) f.typing = false;
                    else if (ev.key == term::K_BACKSPACE) {
                        while (!f.buf.empty() && ((unsigned char)f.buf.back() & 0xC0) == 0x80) f.buf.pop_back();   // a whole UTF-8 letter
                        if (!f.buf.empty()) f.buf.pop_back();
                    } else if (ev.key >= 32 && ev.key != 127 && f.buf.size() < 200) {
                        if (ev.key < 128) f.buf += (char)ev.key;
                        else if (ev.key < 0x800) f.buf += (char)(0xC0 | (ev.key >> 6)), f.buf += (char)(0x80 | (ev.key & 0x3F));
                        else f.buf += (char)(0xE0 | (ev.key >> 12)), f.buf += (char)(0x80 | ((ev.key >> 6) & 0x3F)), f.buf += (char)(0x80 | (ev.key & 0x3F));
                    }
                } else {
                    int before = f.row;
                    if (key == "up") f.row = (f.row + F_COUNT_ - 1) % F_COUNT_;
                    else if (key == "down" || key == "tab") f.row = (f.row + 1) % F_COUNT_;
                    else if (key == "left" || key == "right") {
                        if (f.row == F_TEMPO || (f.row >= F_BARS && f.row <= F_GRID)) {
                            step_field(f.s, f.row, key == "left" ? -1 : 1);
                            f.dirty = true;
                        }
                    } else if (ev.key == '/' && f.row == F_TEMPO) f.s.bpm = std::max(30.0, f.s.bpm / 2), f.dirty = true;
                    else if (ev.key == '*' && f.row == F_TEMPO) f.s.bpm = std::min(300.0, f.s.bpm * 2), f.dirty = true;
                    else if (key == "enter") {
                        if (text_field(f.row)) {
                            f.typing = true;
                            if (f.row == F_TEMPO) {
                                char b[32];
                                std::snprintf(b, sizeof b, "%g", f.s.bpm);
                                f.buf = b;
                            } else
                                f.buf = *text_of(f.s, f.row);
                        } else if (f.row == F_BEAT) {
                            pick.reset(beats_picker(cfg, false));
                            pick->action = "form_style";
                            pick->kind = "formbeat";
                            pick->sel = 0;
                            for (size_t i = 0; i < pick->items.size(); i++)
                                if (pick->items[i].first == f.s.style) pick->sel = (int)i;
                        } else if (f.row == F_KIT) {
                            pick.reset(new Picker{"Kit for this song", {{"", "(keep the kit that's on)"}}, 0, "form_kit", "formkit"});
                            for (const auto &i : kit::list(paths::kits())) {
                                if (i.slug == f.s.kit) pick->sel = (int)pick->items.size();
                                pick->items.push_back({i.slug, i.name + "  (" + i.style + ")"});
                            }
                        } else if (f.row == F_LYRICS) {
                            if (edit_in_editor(f.s.lyrics)) f.dirty = true;
                            else ctl.note("Couldn't open a text editor for the lyrics (set EDITOR, or edit them on the iPad).");
                        } else if (f.row >= F_BARS && f.row <= F_GRID) {
                            step_field(f.s, f.row, 1);
                            f.dirty = true;
                        }
                    } else if (key == "u") {   // what's playing now: tempo, beat, kit, and feel
                        f.s.bpm = cfg.bpm;
                        if (pattern::find(cfg.style) && cfg.style != "edit-preview") f.s.style = cfg.style;
                        f.s.kit = cfg.kit;
                        f.s.swing = cfg.swing;
                        f.s.pocket = cfg.pocket_ms;
                        f.s.loose = cfg.humanize;
                        f.s.locked = cfg.locked ? 1 : 0;
                        f.dirty = true;
                        ctl.note("Filled in from what's playing. s saves it.");
                    } else if (key == "b") {   // edit this song's beat in the beat builder
                        if (const pattern::Style *st = pattern::find(f.s.style)) open_builder(st);
                    } else if (key == k("play")) ctl.act("play");   // the drums still start and stop
                    else if (key == "s") {
                        if (save_form()) form.reset();
                    } else if (key == "esc" || key == "q") {
                        if (f.dirty && !f.confirm_leave) f.confirm_leave = true;
                        else form.reset();
                    }
                    if (form && f.row != before) f.confirm_leave = false;
                }
            } else if (pick) {
                int n = (int)pick->items.size();
                std::string val = n ? pick->items[(size_t)pick->sel].first : "";
                if (key == "up") pick->sel = (pick->sel + n - 1) % std::max(1, n);
                else if (key == "down") pick->sel = (pick->sel + 1) % std::max(1, n);
                else if (pick->kind == "look" && key == "enter" && n) {   // keep it (and stay, to pick the lights too)
                    ctl.act("look", val);
                    try_theme.clear();
                    try_lights.clear();
                    int sel = pick->sel;
                    pick.reset(look_picker(cfg));
                    pick->sel = sel;
                } else if (pick->kind == "look" && (key == "esc" || key == "q")) {
                    try_theme.clear();
                    try_lights.clear();
                    pick.reset();
                }
                else if (key == "enter" && n && form && (pick->action == "form_style" || pick->action == "form_kit")) {
                    if (pick->action == "form_style") form->s.style = val;
                    else form->s.kit = val;
                    form->dirty = true;
                    pick.reset();
                } else if (pick->kind == "songs" && key == "e" && n) {
                    auto list = ctl.set_songs();
                    int i = std::atoi(val.c_str());
                    if (i >= 0 && i < (int)list.size()) {
                        open_form(&list[(size_t)i]);
                        pick.reset();
                    }
                } else if (pick->kind == "songs" && key == "n") {
                    open_form(nullptr);
                    pick.reset();
                } else if (key == "enter" && n) {
                    if (val == "*all") pick.reset(beats_picker(cfg, false));
                    else {
                        ctl.act(pick->action, val);
                        pick.reset();
                    }
                } else if ((pick->kind == "beats" || pick->kind == "fav") && key == "f" && n && val != "*all") {
                    auto it = std::find(cfg.favorites.begin(), cfg.favorites.end(), val);
                    if (it == cfg.favorites.end()) cfg.favorites.push_back(val);
                    else cfg.favorites.erase(it);
                    config::save(cfg);
                    int sel = pick->sel;
                    pick.reset(beats_picker(cfg, pick->kind == "fav"));
                    pick->sel = std::min(sel, (int)pick->items.size() - 1);
                } else if ((pick->kind == "beats" || pick->kind == "fav") && key == "e" && n && val != "*all") {
                    open_builder(pattern::find(val));
                    pick.reset();
                } else if (pick->kind == "beats" && key == "d" && n) {
                    const pattern::Style *s = pattern::find(val);
                    if (s && s->user) {
                        pattern::delete_user(beats_dir, val);
                        ctl.note("Deleted \"" + s->name + "\"");
                        int sel = pick->sel;
                        pick.reset(beats_picker(cfg, false));
                        pick->sel = std::min(sel, (int)pick->items.size() - 1);
                    } else
                        ctl.note("Only your own beats can be deleted.");
                } else if (key == "esc" || key == "q") pick.reset();
                if (pick && pick->kind == "look" && !pick->items.empty()) {
                    const std::string &v = pick->items[(size_t)pick->sel].first;
                    if (v.rfind("theme:", 0) == 0) try_theme = v.substr(6), try_lights.clear();
                    else try_lights = v.substr(7), try_theme.clear();
                }
            } else if (help) {
                help = false;
            } else {
                std::string action;
                for (const auto &kv : cfg.keys)
                    if (kv.second == key) action = kv.first;
                if (key == "=") action = "tempo_up";
                main_action(action);
            }
        }
        // ---- draw
        apply_look(try_theme.empty() ? cfg.tui_theme : try_theme, try_lights.empty() ? cfg.tui_lights : try_lights);
        int W = term::width(), H = term::height();
        g_hots.clear();
        term::clear(BG);
        Setup setup = ctl.setup();
        bool healthy = ctl.engine().healthy();
        bool pedal_ok = setup.looper && setup.dev.midi_card >= 0 && healthy;
        std::string addr = ctl.bookmark().empty() ? ctl.address() : ctl.bookmark();
        wordmark(2, 0);
        {
            // the right: is the pedal there, and where the iPad finds Barline
            std::string c = std::string(term::ascii() ? "* " : "\xE2\x97\x8F ") + connection(setup, healthy);
            int rx = W - 2 - text_w(c);
            term::put(rx, 0, c, pedal_ok ? GOOD : BAD, BG, term::A_BOLD);
            if (!addr.empty() && ctl.lan) {
                net::Net n = ctl.network();
                std::string a = (n.kind == "usb" && !n.ip.empty() ? "USB router  " : "") + std::string("iPad ") + addr.substr(addr.find("//") + 2) + "  code " +
                                cfg.web_code;
                if (rx - text_w(a) - 4 > 14) {
                    rx -= text_w(a) + 4;
                    term::put(rx, 0, a, DIM, BG);
                }
            }
            std::string sp = " LAPTOP SPEAKERS ON ";
            if (ctl.preview && 11 + text_w(sp) < rx) term::put(11, 0, sp, HI, WARN_BG, term::A_BOLD);
        }
        engine::Status s = ctl.engine().status();
        auto draw_pick = [&] {
        if (pick) {
                int w = std::min(W - 4, 76), h = std::min(H - 4, (int)pick->items.size() + 3);
                int x0 = (W - w) / 2, y0 = (H - h) / 2;
                box(x0, y0, w, h, pick->title);
                int rows = h - 3, top = std::max(0, std::min(pick->sel - rows / 2, (int)pick->items.size() - rows));
                for (int i = 0; i < rows && top + i < (int)pick->items.size(); i++) {
                    bool on = top + i == pick->sel;
                    std::string t = pick->items[(size_t)(top + i)].second;
                    term::fill(x0 + 1, y0 + 1 + i, w - 2, 1, on ? SEL : POPUP);
                    if (on) term::put(x0 + 1, y0 + 1 + i, term::ascii() ? ">" : "\xE2\x96\x8C", ACCENT, SEL);
                    term::put(x0 + 3, y0 + 1 + i, t.substr(0, (size_t)w - 5), on ? HI : FG, on ? SEL : POPUP, on ? term::A_BOLD : 0);
                }
                std::string hint = pick->kind == "beats" ? "enter pick  f favorite  e edit a copy  d delete yours  esc close"
                                 : pick->kind == "fav"   ? "enter pick  f unfavorite  e edit a copy  esc close"
                                 : pick->kind == "songs" ? "enter play it  e edit  n new song  esc close"
                                                         : "enter pick  esc close";
                term::put(x0 + 2, y0 + h - 2, hint, DIM, POPUP);
            }
        };
        if (form && !build) {
            // ---- the song editor
            SongForm &f = *form;
            term::put(2, 2, f.s.id.empty() ? "NEW SONG" : "EDIT SONG", HEAD, BG, term::A_BOLD);
            term::put(14, 2, f.s.name + (f.dirty ? "  (not saved)" : ""), FG, BG, term::A_BOLD);
            int y0 = 4;
            for (int i = 0; i < F_COUNT_ && y0 + i < H - 5; i++) {
                bool on = i == f.row;
                int y = y0 + i;
                term::fill(2, y, W - 4, 1, on ? SEL : BG);
                term::put(3, y, field_label(i), on ? HI : DIM, on ? SEL : BG, on ? term::A_BOLD : 0);
                std::string v = on && f.typing ? f.buf + "_" : field_value(f.s, i);
                term::put(18, y, v.substr(0, (size_t)std::max(0, W - 22)), on ? (f.typing ? ACCENT : HI) : FG, on ? SEL : BG, on ? term::A_BOLD : 0);
            }
            std::string help1 = f.typing ? "type, enter keeps it, esc cancels"
                              : text_field(f.row) ? "enter type it"
                              : f.row == F_BEAT || f.row == F_KIT ? "enter pick from the list"
                              : f.row == F_LYRICS ? "enter edit the lyrics in your text editor"
                                                  : "left/right change it";
            if (!f.typing && f.row == F_TEMPO) help1 += "   left/right +/-1   / half   * double";
            if (f.confirm_leave) term::put(2, H - 5, "Not saved. Press s to save, or Esc again to leave without saving.", BAD, BG, term::A_BOLD);
            int hx = chip(2, H - 3, "up down", "move", W - 1);
            term::put(hx, H - 3, help1, DIM, BG);
            hx = chip(2, H - 2, "u", "use what's playing (tempo, beat, kit, feel)", W - 1);
            chip(hx, H - 2, "b", "edit this song's beat", W - 1);
            hx = chip(2, H - 1, "s", "save", W - 1);
            hx = chip(hx, H - 1, "esc", "leave", W - 1);
            chip(hx, H - 1, k("play"), "drums still start and stop", W - 1);
            draw_pick();
            term::present();
            continue;
        }
        if (build) {
            // ---- the beat builder
            Builder &b = *build;
            const pattern::Style &w = b.work;
            const pattern::Part &part = w.parts[b.section];
            auto lanes = pattern::drum_names();
            term::put(2, 2, "BEAT BUILDER", HEAD, BG, term::A_BOLD);
            term::put(17, 2, (b.naming ? b.name_buf + "_" : w.name) + (b.dirty ? "  (not saved)" : ""), b.naming ? ACCENT : FG, BG, term::A_BOLD);
            char info[160];
            std::snprintf(info, sizeof info, "%s   %d BPM   swing %d%%", w.meter.c_str(), w.bpm, (int)std::lround(w.swing * 100));
            term::put(2, 3, info, DIM, BG);
            int x = 2;
            for (int sec = 0; sec < pattern::SECTIONS; sec++) {
                std::string t = std::string(" ") + pattern::part_label(sec) + " ";
                term::put(x, 4, t, sec == b.section ? ON_ACC : FG, sec == b.section ? ACCENT : RAISED, sec == b.section ? term::A_BOLD : 0);
                x += (int)t.size() + 1;
            }
            term::put(x + 1, 4, part.bars == 2 ? "2 bars" : "1 bar", DIM, BG);
            int cols = w.steps * std::max(1, part.bars), per = w.steps / std::max(1, w.quarters);
            int rows = std::max(3, H - 10);
            if (b.row < b.top) b.top = b.row;
            if (b.row >= b.top + rows) b.top = b.row - rows + 1;
            for (int r = 0; r < rows && b.top + r < (int)lanes.size(); r++) {
                int li = b.top + r, y = 6 + r;
                int note = lanes[(size_t)li].second;
                bool used = false;
                for (const auto &h : part.hits)
                    if (h.note == note) used = true;
                term::put(2, y, lanes[(size_t)li].first, li == b.row ? ACCENT : used ? FG : DIM, BG, li == b.row ? term::A_BOLD : 0);
                int cx = 14;
                for (int c = 0; c < cols; c++) {
                    if (c && c % per == 0) cx++;
                    if (c && c % w.steps == 0) cx++;
                    int v = hit_at(part, c, note);
                    std::string glyph = v >= 115 ? "X" : v >= 88 ? "x" : v >= 58 ? "o" : v ? "g" : (c % per == 0 ? "\xC2\xB7" : ".");
                    bool cur = li == b.row && c == b.col;
                    int fg = v ? (v >= 115 ? HI : v >= 88 ? ACCENT : v >= 58 ? DIM : FAINT) : LINE;
                    term::put(cx, y, glyph, cur ? ON_ACC : fg, cur ? ACCENT : (c / per) % 2 ? PANEL : BG, v ? term::A_BOLD : 0);
                    cx++;
                }
            }
            std::string lights;
            for (int q = 1; q <= s.beats; q++) lights += (s.grid && q == s.beat) ? "\xE2\x97\x8F " : "\xE2\x97\x8B ";
            term::put(2, H - 4, lights + (s.playing ? " playing" : " stopped"), s.playing ? GOOD : DIM, BG);
            if (b.confirm_leave) term::put(2, H - 5, "Not saved. Press s to save, or Esc again to leave without saving.", BAD, BG, term::A_BOLD);
            int hx = chip(2, H - 3, "up down left right", "move", W - 1);
            hx = chip(hx, H - 3, "space", "hit", W - 1);
            hx = chip(hx, H - 3, "X", "accent", W - 1);
            hx = chip(hx, H - 3, "o", "soft", W - 1);
            hx = chip(hx, H - 3, "g", "ghost", W - 1);
            hx = chip(hx, H - 3, ".", "clear", W - 1);
            chip(hx, H - 3, "tab", "next part", W - 1);
            hx = chip(2, H - 2, "c", "copy the verse here", W - 1);
            hx = chip(hx, H - 2, "l", "1 or 2 bars", W - 1);
            hx = chip(hx, H - 2, "m", "time signature", W - 1);
            hx = chip(hx, H - 2, "w", "swing", W - 1);
            chip(hx, H - 2, "- +", "tempo", W - 1);
            hx = chip(2, H - 1, "enter", "play / stop (laptop too)", W - 1);
            hx = chip(hx, H - 1, "n", "name", W - 1);
            hx = chip(hx, H - 1, "s", "save", W - 1);
            chip(hx, H - 1, "esc", "leave", W - 1);
            term::present();
            continue;
        }
        bool roomy = H >= 27 && W >= 78;
        int y = 2;
        // ---- the song
        songs::Song cur;
        if (!ctl.current_song.empty() && ctl.library().get(ctl.current_song, cur)) {
            std::string tag = cfg.song >= 0 ? "SONG " + std::to_string(cfg.song + 1) : "SONG";
            term::put(2, y, tag, FAINT, BG, term::A_BOLD);
            term::put(3 + text_w(tag), y, cur.name, HI, BG, term::A_BOLD);
            std::string right = cur.key.empty() ? "" : "key " + cur.key;
            if (!cfg.setlist.empty()) right += (right.empty() ? "" : "    ") + cfg.setlist;
            if (!right.empty() && 6 + text_w(tag) + text_w(cur.name) + text_w(right) < W) term::put(W - 2 - text_w(right), y, right, DIM, BG);
        } else
            chip(2, y, k("songs"), "pick a song  (or just play)", W - 2, "songs");
        y += 2;
        // ---- the tempo (big, for across the stage) and the groove
        int ch = roomy ? 7 : 4, lw = std::min(roomy ? 38 : 32, W / 2);
        card(1, y, lw, ch, "Tempo", ACCENT);
        int bpm = (int)std::lround(s.bpm);
        if (roomy && g_theme->big_tempo) {
            big_number(4, y + 1, bpm, s.grid ? HI : FG);
            term::put(4 + 8 * (int)std::to_string(bpm).size(), y + 3, "BPM", DIM, PANEL, term::A_BOLD);
        } else
            term::put(4, y + 1, std::to_string(bpm) + " BPM", HI, PANEL, term::A_BOLD);
        std::string bl = s.style + (is_fav(cfg, cfg.style) ? (term::ascii() ? " *" : " \xE2\x98\x85") : "") + "  \xC2\xB7  " + s.kit;
        term::put(4, y + ch - 3 + (roomy ? 0 : 1), bl.substr(0, (size_t)std::max(0, lw - 5)), FG, PANEL);
        if (const pattern::Style *st = pattern::find(cfg.style)) {
            int p = (int)std::lround(cfg.pocket_ms);
            int sw = (int)std::lround((cfg.swing < 0 ? st->swing : cfg.swing) * 100);
            std::string feel = std::string(cfg.locked ? "Locked" : "Human") + " \xC2\xB7 swing " + std::to_string(sw) + "%" +
                               (cfg.locked ? "" : " \xC2\xB7 " + (p == 0 ? std::string("on the beat") : p > 0 ? "back " + std::to_string(p) + " ms" : "pushed " + std::to_string(-p) + " ms") +
                                                      " \xC2\xB7 loose " + std::to_string((int)std::lround(cfg.humanize * 100)) + "%");
            if (roomy) term::put(4, y + ch - 2, feel.substr(0, (size_t)std::max(0, lw - 5)), DIM, PANEL);
        }
        {
            int gx = lw + 2, gw = W - gx - 1;
            card(gx, y, gw, ch, "Groove", HEAD);
            // where the song is: the parts, and what's coming
            std::string state = !s.grid ? "STOPPED" : s.counting ? "COUNT-IN" : !s.playing ? "KEEPING TIME" : s.ending ? "ENDING"
                              : s.pause == 2 ? "PAUSE" : s.clap_now ? "CLAP ALONG" : "";
            int px = gx + 10;
            if (s.playing && !s.counting) {
                for (int sec : {pattern::VERSE, pattern::CHORUS}) {
                    bool on = s.section == sec && !s.ending, next = s.next_section == sec;
                    std::string t = std::string(" ") + upper(pattern::section_name(sec)) + " ";
                    term::put(px, y, t, on ? ON_ACC : next ? ACCENT : DIM, on ? ACCENT : RAISED, on || next ? term::A_BOLD : 0);
                    px += text_w(t) + 1;
                }
                if (s.next_section >= 0) term::put(px, y, "fill, then " + std::string(pattern::section_name(s.next_section)), ACCENT, PANEL);
                else if (s.pause == 1) term::put(px, y, "pause coming", ACCENT, PANEL);
                else if (s.clapping && !s.clap_now) term::put(px, y, "claps next bar", ACCENT, PANEL);
            }
            std::string right = state + (s.grid ? (state.empty() ? "" : "   ") + std::string("bar ") + std::to_string(s.bar) : "");
            if (gx + gw - 2 - text_w(right) > px) term::put(gx + gw - 2 - text_w(right), y, right, s.counting || s.ending || s.pause == 2 ? ACCENT : FG, PANEL, term::A_BOLD);
            beat_lights(s, gx, y, gw, ch, roomy);
        }
        y += ch + 1;
        // ---- the loops: A and B as tiles, the same as on the iPad
        int foot_rows = H >= 20 ? 3 : 1;
        if (y + (roomy ? 5 : 3) > H - foot_rows - 1) {   // a tiny window: one line
            std::string l = "A: " + std::string(s.playing_track == 0 ? "playing" : s.has_a ? "ready" : "-") + "   B: " +
                            (s.playing_track == 1 ? "playing" : s.has_b ? "ready" : "-") + (s.loop.empty() ? "" : "   " + s.loop);
            if (y < H - foot_rows - 1) term::put(2, y, l.substr(0, (size_t)std::max(0, W - 4)), s.loop.empty() ? FG : REC, BG, term::A_BOLD);
            y = H;
        } else {
            int lh = roomy ? 5 : 3;
            card(1, y, W - 2, lh, "Loops", B_COL);
            bool songish = cfg.pedal_mode == "song" || cfg.pedal_mode == "off";
            int tw = std::max(ON_ACC, std::min(28, (W - 8) / 3));
            for (int t = 0; t < 2; t++) {
                std::string T = t ? "B" : "A";
                bool rec = s.loop.rfind("Recording " + T, 0) == 0 || s.loop.rfind("Overdub " + T, 0) == 0 || s.loop.rfind("Overdubbing " + T, 0) == 0;
                bool layering = rec && s.loop.rfind("Overdub", 0) == 0;
                bool next = s.next_track == t, playing = s.playing_track == t && !next;
                bool has = t ? s.has_b : s.has_a, empty = t ? s.empty_b : s.empty_a;
                bool off = t == 1 && cfg.pedal_mode == "single";
                bool queued = !rec && s.loop.find("\xC2\xB7 " + T + " next") != std::string::npos;   // records the moment the other closes
                std::string label = off ? "Single mode" : rec ? (layering ? "\xE2\x97\x8F LAYERING" : "\xE2\x97\x8F RECORDING") : queued ? "RECORDS NEXT" : next ? "NEXT BAR" : playing ? "\xE2\x96\xB6 PLAYING"
                                  : has ? "Ready" : empty ? "Empty" : songish ? "Tap to play" : "";
                if (term::ascii()) label = off ? "Single mode" : rec ? (layering ? "* LAYERING" : "* RECORDING") : next ? "NEXT BAR" : playing ? "> PLAYING" : label;
                int col = t ? B_COL : A_COL;
                int bg = off ? PANEL : rec ? REC : playing ? col : RAISED, fg = off ? FAINT : rec ? HI : playing ? ON_ACC : next || queued ? ACCENT : FG;
                int x0 = 4 + t * (tw + 2), h = lh - 2;
                term::fill(x0, y + 1, tw, h, bg);
                term::put(x0 + 1, y + 1, T, playing || rec ? fg : col, bg, term::A_BOLD);
                term::put(x0 + 4, y + 1, label, fg, bg, term::A_BOLD);
                if (h > 1) {
                    int n = ctl.bars_for(t);
                    std::string sub = std::to_string(n) + (n == 1 ? " bar" : " bars");
                    term::put(x0 + 4, y + 2, sub, playing ? ON_ACC : rec ? HI : DIM, bg);
                    if (ctl.track == t && !off) term::put(x0 + tw - 8, y + 2, "\xE2\x97\x86 keys", playing || rec ? fg : ACCENT, bg);
                }
                for (int r = 0; r < h; r++) g_hots.push_back({x0, y + 1 + r, tw, "select", T});
            }
            int ix = 4 + 2 * (tw + 2) + 1, iw = W - 2 - ix;
            if (iw > 18) {
                std::string mode = cfg.pedal_mode == "off" ? "Song mode (not set)" : upper(cfg.pedal_mode.substr(0, 1)) + cfg.pedal_mode.substr(1) + " mode";
                term::put(ix, y + 1, ("Pedal: " + mode).substr(0, (size_t)iw), DIM, PANEL);
                if (!s.loop.empty()) term::put(ix, y + 2, s.loop.substr(0, (size_t)iw), REC, PANEL, term::A_BOLD);
                else if (!s.pedal_state.empty() && s.pedal_state != "unknown") term::put(ix, y + 2, ("The pedal: " + s.pedal_state).substr(0, (size_t)iw), FAINT, PANEL);
                if (roomy && s.loop.empty() && cfg.pedal_mode != "single" && !s.has_a && !s.has_b)
                    chip(ix, y + 3, k("record_ab"), "record A (" + std::to_string(ctl.bars_for(0)) + ") \xE2\x86\x92 B (" + std::to_string(ctl.bars_for(1)) + ")" +
                                                        (songish ? ", then A plays" : ""), W - 2, "record_ab", PANEL);
            }
            y += lh + 1;
        }
        // ---- what happened lately
        int foot = H - foot_rows - (s.problem.empty() ? 0 : 1);
        if (y < foot - 1) {
            term::put(2, y++, "LATELY", FAINT, BG, term::A_BOLD);
            std::lock_guard<std::mutex> lock(ctl.mu());
            size_t n = (size_t)std::max(0, foot - y - 1);
            size_t from = ctl.recent.size() > n ? ctl.recent.size() - n : 0;
            for (size_t i = from; i < ctl.recent.size(); i++) {
                bool newest = i + 1 == ctl.recent.size();
                term::put(2, y, term::ascii() ? ">" : "\xE2\x80\xBA", newest ? HEAD : FAINT, BG);
                term::put(4, y++, ctl.recent[i].substr(0, (size_t)std::max(0, W - 6)), newest ? FG : DIM, BG);
            }
        }
        if (!s.problem.empty()) {
            term::fill(0, H - foot_rows - 1, W, 1, PROB_BG);
            term::put(2, H - foot_rows - 1, "! " + s.problem, PROB_FG, PROB_BG, term::A_BOLD);
        }
        // ---- the keys, as keycaps (click one, or press it)
        if (foot_rows == 1) {
            int x = chip(2, H - 1, k("play"), s.playing && !s.ending ? "end" : "start", W - 1, "play");
            x = chip(x, H - 1, k("smart"), "record", W - 1, "smart");
            x = chip(x, H - 1, k("help"), "keys", W - 1, "help");
            chip(x, H - 1, k("quit"), "quit", W - 1, "quit");
        } else {
            int x;
            term::put(2, H - 3, "DRUMS", FAINT, BG, term::A_BOLD);
            x = 10;
            x = chip(x, H - 3, k("play"), s.playing && !s.ending ? "end" : "start", W - 1, "play");
            x = chip(x, H - 3, k("next_part"), "next part", W - 1, "next_part");
            x = chip(x, H - 3, k("pause_bar"), "pause", W - 1, "pause_bar");
            x = chip(x, H - 3, k("clap"), "clap", W - 1, "clap");
            x = chip(x, H - 3, k("tap"), "tap", W - 1, "tap");
            x = chip(x, H - 3, k("tempo_down") + " " + k("tempo_up"), "tempo", W - 1);
            x = chip(x, H - 3, k("click_toggle"), "click", W - 1, "click_toggle");
            x = chip(x, H - 3, k("end"), "ending", W - 1, "end");
            term::put(2, H - 2, "LOOPS", FAINT, BG, term::A_BOLD);
            x = 10;
            x = chip(x, H - 2, k("smart"), "record / layer", W - 1, "smart");
            x = chip(x, H - 2, k("record_ab"), "A\xE2\x86\x92" "B", W - 1, "record_ab");
            x = chip(x, H - 2, k("track_b"), "A/B", W - 1, "track_b");
            x = chip(x, H - 2, k("undo"), "undo", W - 1, "undo");
            x = chip(x, H - 2, k("stop_loops"), "stop", W - 1, "stop_loops");
            x = chip(x, H - 2, k("bars_down") + " " + k("bars_up"), "length", W - 1);
            x = chip(x, H - 2, k("clear"), "clear", W - 1, "clear");
            term::put(2, H - 1, "MORE", FAINT, BG, term::A_BOLD);
            x = 10;
            x = chip(x, H - 1, k("songs"), "songs", W - 1, "songs");
            x = chip(x, H - 1, k("beats"), "beats", W - 1, "beats");
            x = chip(x, H - 1, k("kits"), "kits", W - 1, "kits");
            x = chip(x, H - 1, k("builder"), "build a beat", W - 1, "builder");
            x = chip(x, H - 1, k("preview"), "hear on laptop", W - 1, "preview");
            x = chip(x, H - 1, k("look"), "look", W - 1, "look");
            x = chip(x, H - 1, k("help"), "all keys", W - 1, "help");
            x = chip(x, H - 1, k("quit"), "quit", W - 1, "quit");
            (void)x;
        }
        draw_pick();
        if (help) {
            // every key, by what it's for, and the few things worth knowing
            int w = std::min(W - 2, 100), h = std::min(H - 1, 30);
            int x0 = (W - w) / 2, y0 = (H - h) / 2;
            box(x0, y0, w, h, "Keys & tips");
            struct L { std::string key, label; };
            std::vector<std::pair<std::string, std::vector<L>>> groups = {
                {"Drums", {{k("play"), "start / end the song"}, {k("end"), "ending now"}, {k("next_part"), "next part (fill first)"}, {k("pause_half") + " " + k("pause_bar"), "pause half / whole bar"},
                           {k("clap"), "clap along"}, {k("tap"), "tap the tempo"}, {k("tempo_down") + " " + k("tempo_up"), "tempo down / up"}, {k("click_toggle"), "headphone click"},
                           {k("preview"), "hear on laptop"}, {k("panic"), "panic: stop everything"}}},
                {"Feel", {{k("lock"), "locked / human"}, {k("swing_step"), "swing"}, {k("pocket_down") + " " + k("pocket_up"), "pocket: pushed / laid back"}, {k("loose_down") + " " + k("loose_up"), "looseness"}}},
                {"Loops", {{k("smart"), "record, then layer on"}, {k("record"), "record"}, {k("overdub"), "overdub"}, {k("record_ab"), "record A then B, then A plays"},
                           {k("track_b"), "track A / B"}, {k("bars_down") + " " + k("bars_up"), "loop length"}, {k("undo"), "undo"}, {k("clear"), "clear the track"}, {k("stop_loops"), "stop loops (on the bar)"}}},
                {"Songs & sounds", {{k("songs"), "songs (e edit, n new)"}, {k("prev_song") + " " + k("next_song"), "previous / next song"}, {k("beats"), "favorite beats"}, {k("all_beats"), "all beats"},
                                    {k("builder"), "build a beat"}, {k("kits"), "drum kits"}, {k("look"), "the look: themes, beat lights"}, {k("window"), "open the window"}, {k("quit"), "quit"}}}};
            int cols = w >= 90 ? 2 : 1, colw = (w - 4) / cols;
            int cx = x0 + 2, cy = y0 + 2, col = 0, top = cy;
            int bottom = y0 + h - 5;
            for (const auto &g : groups) {
                if (cy + 1 + (int)g.second.size() > bottom && col + 1 < cols) {
                    col++;
                    cx = x0 + 2 + col * colw;
                    cy = top;
                }
                term::put(cx, cy++, upper(g.first), HEAD, POPUP, term::A_BOLD);
                for (const auto &l : g.second) {
                    if (cy > bottom) break;
                    caps(cx, cy, l.key);
                    term::put(cx + 12, cy++, l.label, FG, POPUP);
                }
                cy++;
            }
            std::vector<std::string> tips = {"Loops start on the bar line and close exactly their length later: no timing needed.",
                                             "The mouse works too: click a keycap or a loop tile.   Any key closes this."};
            for (int i = 0; i < 2; i++) term::put(x0 + 2, y0 + h - 3 + i, tips[(size_t)i].substr(0, (size_t)std::max(0, w - 4)), DIM, POPUP);
        }
        term::present();
    }
    term::stop();
    return 0;
}

}  // namespace app
