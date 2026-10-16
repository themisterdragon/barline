// Barline: drums, click, and loop timing for a USB looper pedal.
//   barline                 the live screen (in a terminal; without one, the window)
//   barline window          the same in a window of its own (the iPad page, on this computer)
//   barline headless        no screen at all: the iPad page only (a Raspberry Pi at the side of the stage)
//   barline doctor          checks the setup: the pedal, sound, MIDI, the iPad page
//   barline devices         sound and MIDI devices this computer has
//   barline render STYLE KIT SECONDS OUT.wav [--bpm N] [--loop BARS]   (a test, no devices)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>

#include "app.h"
#include "config.h"
#include "doctor.h"
#include "engine.h"
#include "http.h"
#include "kit.h"
#include "paths.h"
#include "pattern.h"
#include "snd.h"
#include "setlist_import.h"
#include "songs.h"
#include "sys.h"
#include "version.h"
#include "web.h"

static int devices() {
    auto cards = snd::cards();
    if (cards.empty()) std::printf("No sound devices found.\n");
    for (const auto &c : cards) {
        std::printf("card %d: %s [%s]\n", c.index, c.name.c_str(), c.id.c_str());
        for (int d : c.play) std::printf("    plays sound      (device %d)\n", d);
        for (int d : c.record) std::printf("    records sound    (device %d)\n", d);
        for (int d : c.midi) std::printf("    MIDI             (device %d)\n", d);
    }
    return 0;
}

static int render(int argc, char **argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: barline render STYLE KIT SECONDS OUT.wav [--bpm N] [--loop BARS]\n");
        return 2;
    }
    std::string style = argv[2], slug = argv[3], out = argv[5];
    double secs = std::atof(argv[4]);
    engine::Engine e;
    engine::Settings s;
    pattern::load_user(paths::data() + "/beats");
    const pattern::Style *st = pattern::find(style);
    if (!st) {
        std::fprintf(stderr, "no style %s\n", style.c_str());
        return 2;
    }
    s.bpm = st->bpm;
    int loop_bars = 0;
    for (int i = 6; i < argc; i++) {
        if (!std::strcmp(argv[i], "--bpm") && i + 1 < argc) s.bpm = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--loop") && i + 1 < argc) loop_bars = std::atoi(argv[++i]);
    }
    std::string err;
    auto k = kit::load(paths::kits(), slug, &err);
    if (!k) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    e.set_settings(s);
    e.set_style(style);
    e.set_kit(k);
    e.play();
    if (loop_bars) e.record_loop(loop_bars);
    std::vector<float> audio;
    std::vector<engine::Engine::MidiOut> midi;
    size_t frames = (size_t)(secs * kit::RATE);
    // halfway through: go to the chorus (a fill first)
    e.render(frames / 2, audio, &midi);
    std::vector<float> more;
    e.next_part();
    e.render(frames - frames / 2, more, &midi);
    audio.insert(audio.end(), more.begin(), more.end());
    if (!kit::write_wav(out, audio)) {
        std::fprintf(stderr, "couldn't write %s\n", out.c_str());
        return 1;
    }
    int clocks = 0;
    for (const auto &m : midi) {
        if (m.bytes.size() == 1 && m.bytes[0] == 0xF8) {
            clocks++;
            continue;
        }
        std::printf("%8.4fs ", m.at);
        for (uint8_t b : m.bytes) std::printf(" %02X", b);
        std::printf("\n");
    }
    std::printf("%d clock pulses (%.1f quarter notes); kit %s, %.1f MB\n", clocks, clocks / 24.0, k->name.c_str(), k->bytes() / 1e6);
    return 0;
}

static volatile std::sig_atomic_t g_quit = 0;
static void on_quit(int) { g_quit = 1; }

static std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool write_private(const std::string &path, const std::string &text) { return sys::write_private(path, text); }

static std::string random_hex(int bytes) {
    std::random_device rd;
    static const char *H = "0123456789abcdef";
    std::string o;
    for (int i = 0; i < bytes; i++) {
        unsigned v = rd() & 255;
        o += H[v >> 4];
        o += H[v & 15];
    }
    return o;
}

static int usage() {
    std::printf("Barline %s: drums, a click, and perfectly timed loops for a USB looper pedal.\n\n"
                "  barline            the live screen (in a terminal; started without one, it opens the window)\n"
                "  barline window     Barline in a window of its own\n"
                "  barline headless   no screen: only the iPad page (for a computer with no screen)\n"
                "  barline doctor     check the setup: the pedal, sound, MIDI, the iPad page\n"
                "  barline devices    the sound and MIDI devices this computer has\n"
                "  barline --version\n\n"
                "Settings, songs, and beats: %s\n",
                BARLINE_VERSION, paths::data().c_str());
    return 0;
}

int main(int argc, char **argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "devices") return devices();
    if (cmd == "render") return render(argc, argv);
    if (cmd == "doctor") return doctor::run(argc > 2 && !std::strcmp(argv[2], "--fix"));
    if (cmd == "--version" || cmd == "-v") {
        std::printf("Barline %s\n", BARLINE_VERSION);
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return usage();
    bool window = cmd == "window" || cmd == "--window";
    bool headless = cmd == "headless";
    if (!cmd.empty() && !window && !headless) {
        std::fprintf(stderr, "Barline doesn't know \"%s\".\n\n", cmd.c_str());
        usage();
        return 2;
    }
    // without a terminal (started from the app menu): the window
    if (!window && !headless && !sys::is_terminal()) window = true;
    // Windows: a double-click (in Explorer, or the Start menu) opens the window; typed in a terminal, the screen
    if (!window && !headless && cmd.empty() && sys::double_clicked()) window = true;

    // one Barline at a time (two would fight over the pedal): a second one opens the first one's window
    std::string data = paths::data();
    if (!sys::take_lock(data + "/barline.lock")) {
        std::string url = read_file(data + "/window.url");
        if (!headless && !url.empty() && http::open_app_window(url, data + "/window-profile")) {
            std::printf("Barline is already running. Opened its window.\n");
            return 0;
        }
        std::fprintf(stderr, "Barline is already running (in another terminal or window). Quit that one first.\n");
        return 1;
    }
    app::log_line("Barline " BARLINE_VERSION " started" + std::string(window ? " (window)" : headless ? " (headless)" : ""));

    // the live screen
    pattern::load_user(data + "/beats");
    config::Config cfg = config::load();
    config::save(cfg);
    songs::Library lib(data);
    lib.load();
    // song settings an update brought (estimated tempos and beats): applied once, then set aside
    std::vector<std::string> applied;
    {
        std::string pend = data + "/pending";
        if (sys::is_dir(pend)) {
            std::vector<std::string> files;
            for (const auto &n : sys::list_dir(pend))
                if (n[0] != '.' && n.find(".csv") != std::string::npos) files.push_back(n);
            std::sort(files.begin(), files.end());
            sys::make_dir(pend + "/done", 0700);
            // beats that were renamed: "old-id new-id" a line (beat-ids*.txt), for your songs, favorites, and the beat that's on
            {
                std::vector<std::string> ren;
                for (const auto &n : sys::list_dir(pend))
                    if (n.compare(0, 8, "beat-ids") == 0 && n.find(".txt") != std::string::npos) ren.push_back(n);
                for (const auto &f : ren) {
                    std::map<std::string, std::string> to;
                    std::istringstream in(read_file(pend + "/" + f));
                    std::string a, b;
                    while (in >> a >> b) to[a] = b;
                    int n = 0;
                    for (auto song : lib.all())
                        if (to.count(song.style)) {
                            song.style = to[song.style];
                            lib.save(song);
                            n++;
                        }
                    for (auto &fav : cfg.favorites)
                        if (to.count(fav)) fav = to[fav];
                    if (to.count(cfg.style)) cfg.style = to[cfg.style];
                    config::save(cfg);
                    applied.push_back("Renamed beats: " + std::to_string(n) + " songs updated");
                    sys::replace_file(pend + "/" + f, pend + "/done/" + f);
                }
            }
            for (const auto &f : files) {
                setlist_import::Result r = setlist_import::import(read_file(pend + "/" + f), lib);
                applied.push_back(r.error.empty() ? "Filled in " + std::to_string(r.songs) + " songs from " + f : f + ": " + r.error);
                sys::replace_file(pend + "/" + f, pend + "/done/" + f);
            }
        }
    }
    if (!cfg.songs.empty()) {   // an old settings file's songs: into the song library and a set list
        songs::SetList l;
        l.name = "My Songs";
        for (const auto &o : cfg.songs) {
            songs::Song s;
            s.name = o.name;
            s.style = o.style;
            s.kit = o.kit;
            s.bpm = o.bpm;
            s.loop_bars = o.loop_bars;
            s.slot = o.slot;
            s.count_in = o.count_in;
            s.notes = o.notes;
            std::string id = lib.save(s);
            if (!id.empty()) l.songs.push_back(id);
        }
        lib.save_setlist(l);
        cfg.setlist = l.name;
        cfg.songs.clear();
    }
    app::Setup setup = app::find_devices(cfg);
    engine::Engine eng;
    app::Control ctl(eng, cfg, lib);
    ctl.window_mode = window;
    ctl.push_settings();
    eng.set_style(cfg.style);
    ctl.load_kit(cfg.kit);
    std::string err;
    // a device that won't open isn't the end: Barline keeps time, says so, and keeps trying
    if (!eng.start(setup.dev, &err) || !err.empty()) ctl.note("Couldn't open " + err);
    ctl.set_setup(setup);
    ctl.note("Drums to: " + (setup.out_name.empty() ? std::string("nowhere") : setup.out_name) +
             "   click to: " + (setup.click_name.empty() ? std::string("off") : setup.click_name));
    for (const auto &n : setup.notes) ctl.note(n);
    for (const auto &n : applied) ctl.note(n);
    if (cfg.song >= 0) ctl.apply_song(cfg.song);
    std::unique_ptr<web::Page> page;
    ctl.lan = cfg.web || headless;   // headless is the iPad page and nothing else, so it's on then
    if (ctl.lan || window) {   // the window is the page, so it's on for the window
        ctl.local_token = random_hex(16);
        page.reset(new web::Page(ctl));
        std::string werr;
        if (page->start(&werr).empty()) {
            ctl.note("The iPad page couldn't start: " + werr);
            page.reset();
        } else {
            ctl.web_port = page->port();
            write_private(data + "/window.url", ctl.window_url());
        }
    }
    ctl.watch_devices();
    if (page && ctl.lan) ctl.note("iPad/phone: open " + (ctl.bookmark().empty() ? ctl.address() : ctl.bookmark()) + " and enter the code " + cfg.web_code);
    int r = 0;
    if (headless) {
        std::signal(SIGINT, on_quit);
        std::signal(SIGTERM, on_quit);
#ifdef SIGHUP
        std::signal(SIGHUP, on_quit);
#endif
        if (!page) r = 1;
        else std::printf("Barline is running with no screen. iPad/phone: open %s and enter the code %s\n",
                         (ctl.bookmark().empty() ? ctl.address() : ctl.bookmark()).c_str(), cfg.web_code.c_str());
        std::fflush(stdout);
        while (!r && !ctl.quit && !g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else if (window) {
        std::signal(SIGINT, on_quit);
        std::signal(SIGTERM, on_quit);
#ifdef SIGHUP
        std::signal(SIGHUP, on_quit);
#endif
        if (!page) {
            std::fprintf(stderr, "Barline's window couldn't start (the page didn't).\n");
            r = 1;
        } else if (!http::open_app_window(ctl.window_url(), data + "/window-profile")) {
            std::fprintf(stderr, "No web browser found to open the window in. Open this in one: %s\n", ctl.window_url().c_str());
            app::log_line("No web browser found for the window.");
            if (!sys::is_terminal()) r = 1;   // nobody can see the address: don't run on unseen
        } else if (sys::is_terminal())
            std::printf("Barline is running in its window. Quit it there (Settings > Quit), or press Ctrl-C here.\n");
        while (!r && !ctl.quit && !g_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (ctl.window_closed()) ctl.quit = true;
        }
    } else
        r = app::run(ctl);
    ctl.stop_watching();
    if (page) page->stop();
    std::remove((data + "/window.url").c_str());
    eng.stop();
    config::save(cfg);
    app::log_line("Barline quit");
    return r;
}
