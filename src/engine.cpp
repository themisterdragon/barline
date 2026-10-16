#include "engine.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <condition_variable>

#include <thread>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "sys.h"

namespace engine {

bool tracing() {
    static const char *path = getenv("BARLINE_TRACE");
    return path && *path;
}

void trace(const std::string &s) {
    if (!tracing()) return;
    static const char *path = getenv("BARLINE_TRACE");
    static std::mutex m;
    static FILE *f = fopen(path, "a");
    if (!f) return;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(m);
    fprintf(f, "%.3f %s\n", ms, s.c_str());
    fflush(f);
}

using clk = std::chrono::steady_clock;

Engine::Engine() {
    voices_.reserve(64);
    style_ = pattern::find("rock");
}

Engine::~Engine() { stop(); }

// ----------------------------------------------------------------------------- devices and threads

bool Engine::start(const Devices &d, std::string *err) {
    stop();
    return reopen(d, err);
}

bool Engine::reopen(const Devices &d, std::string *err) {
    std::lock_guard<std::mutex> guard(laptop_mu_);   // not while the laptop's sound is being switched
    shut(false);
    dev_ = d;
    want_out_ = d.out_card >= 0;
    want_midi_ = d.midi_card >= 0;
    has_laptop_ = d.click_card >= 0 && (d.click_card != d.out_card || d.click_dev != d.out_dev);
    std::string e, problem;
    if (d.out_card >= 0 && !open_at(out_, d.out_card, d.out_dev, 2, d.period, &e)) problem = "drums out: " + e;
    out_rs_.step = out_.is_open() ? (double)kit::RATE / out_.rate : 1;
    out_rs_.pos = 0;
    // the laptop's own sound: only while the click or "hear on laptop" wants it (so the computer's other
    // sound isn't shut out the rest of the time). A nice-to-have: said, but not shown as a problem.
    std::string laptop;
    click_.reset();
    if (laptop_wanted_ && has_laptop()) {
        auto p = std::make_shared<snd::Pcm>();
        if (open_at(*p, d.click_card, d.click_dev, 3, d.period, &e)) {
            click_ = p;
            click_rs_ = Resampler();
            click_rs_.step = (double)kit::RATE / p->rate;
        } else
            laptop = "this computer's own sound: " + e;
    }
    if (d.midi_card >= 0 && !midi_.open(d.midi_card, d.midi_dev, false, true, &e))
        problem += std::string(problem.empty() ? "" : "; ") + "MIDI to the pedal: " + e;
    out_ok_ = out_.is_open();
    midi_ok_ = midi_.is_open();
    click_ok_ = click_ != nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.problem = problem.empty() ? "" : "Couldn't open " + problem;
    }
    if (err) *err = problem + (problem.empty() || laptop.empty() ? "" : "; ") + laptop;
    run_ = true;
    sys::fine_timers();
    midi_thread_ = std::thread(&Engine::midi_loop, this);
    thread_ = std::thread(&Engine::loop_thread, this);
    return problem.empty();
}

void Engine::stop() { shut(true); }

// 44.1 kHz (the pedal's own rate); a device that won't take it gets 48 kHz, converted
bool Engine::open_at(snd::Pcm &p, int card, int devn, unsigned periods, unsigned period, std::string *err) {
    for (unsigned rate : {44100u, 48000u})
        if (p.open(card, devn, false, rate, 2, rate == 44100 ? period : period * 48000 / 44100, periods, err)) return true;
    return false;
}

// a device the audio thread has let go of: closed elsewhere (closing waits for its sound to play out)
void Engine::retire(std::shared_ptr<snd::Pcm> p) {
    if (!p) return;
    std::unique_lock<std::mutex> lock(retire_mu_, std::try_to_lock);
    if (lock.owns_lock()) {
        retired_.push_back(std::move(p));
        if (laptop_leak_) retired_.push_back(std::move(laptop_leak_));
    } else
        laptop_leak_ = std::move(p);   // (never waits in the audio thread; handed over next time)
}

// the laptop's own sound on or off, without a gap in the drums to the pedal: it's opened here, then handed
// to the audio thread (or taken back from it)
bool Engine::set_laptop(bool want, std::string *err) {
    std::lock_guard<std::mutex> guard(laptop_mu_);
    laptop_wanted_ = want;
    {
        std::lock_guard<std::mutex> lock(retire_mu_);
        retired_.clear();   // close what's been let go of (here, not in the audio thread)
    }
    if (!run_) return true;
    if (!want) {
        if (click_ok_) {
            post([this] {
                retire(std::move(click_));
                click_ok_ = false;
            });
        }
        return true;
    }
    if (click_ok_ || !has_laptop()) return true;
    auto p = std::make_shared<snd::Pcm>();
    std::string e;
    if (!open_at(*p, dev_.click_card, dev_.click_dev, 3, dev_.period, &e)) {
        if (err) *err = e;
        return false;
    }
    double step = (double)kit::RATE / p->rate;
    post([this, p, step] {
        retire(std::move(click_));
        click_ = p;
        click_rs_ = Resampler();
        click_rs_.step = step;
        click_ok_ = true;
    });
    return true;
}

// goodbye: the pedal is told the clock has stopped and every note is off (not when only the devices change)
void Engine::shut(bool goodbye) {
    if (!run_.exchange(false)) return;
    midi_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (midi_thread_.joinable()) midi_thread_.join();
    if (goodbye && midi_.is_open()) {
        uint8_t ch = midi_ch();
        midi_.send({0xFC});                       // clock stop
        midi_.send({(uint8_t)(0xB0 | ch), 123, 0});   // all notes off
    }
    out_.close();
    click_.reset();
    midi_.close();
    out_ok_ = midi_ok_ = click_ok_ = false;
    laptop_leak_.reset();
    std::lock_guard<std::mutex> lock(retire_mu_);
    retired_.clear();
}

static void realtime_priority() { sys::audio_thread_priority(); }

void Engine::loop_thread() {
    realtime_priority();
    unsigned n = std::max(64u, std::min(2048u, out_.is_open() ? out_.period : dev_.period));   // an odd device's 1-frame blocks: work in bigger ones
    buf_.assign((size_t)n * 2, 0);
    click_buf_.assign((size_t)n * 2, 0);
    auto next = clk::now();
    bool timed = !out_.is_open();   // keeping time on a timer (no sound device): never run ahead of it
    while (run_) {
        process(buf_.data(), click_buf_.data(), n);
        const float *ob = buf_.data(), *cb = click_buf_.data();
        unsigned on = n, cn = n;
        if (out_.is_open() && out_.rate != (unsigned)kit::RATE) {
            out_rs_.push(buf_.data(), n);
            ob = out_rs_.out.data();
            on = (unsigned)(out_rs_.out.size() / 2);
        }
        if (click_ && click_->rate != (unsigned)kit::RATE) {
            click_rs_.push(click_buf_.data(), n);
            cb = click_rs_.out.data();
            cn = (unsigned)(click_rs_.out.size() / 2);
        }
        if (out_.is_open()) {
            if (on && !out_.write(ob, on)) {
                std::lock_guard<std::mutex> lock(mu_);
                status_.problem = "The drums' sound device went away (unplugged?).";
                out_.close();
                out_ok_ = false;
            }
        } else if (click_) {
            // no pedal, but the laptop's own sound is on: its device sets the pace (its write waits), so the
            // drums never run ahead of it or starve it (a timer here and the device's clock drift apart)
            timed = false;
        } else {   // no sound device: keep time with the clock alone (MIDI still goes out on time)
            if (!timed) {   // it just went away: the timer starts now (not from when the thread started)
                timed = true;
                next = clk::now();
            }
            next += std::chrono::nanoseconds((int64_t)(1e9 * n / kit::RATE));
            std::this_thread::sleep_until(next);
        }
        if (click_ && cn && !click_->write(cb, cn)) {
            retire(std::move(click_));   // the laptop's sound went away: the drums to the pedal carry on
            click_ok_ = false;
        }
    }
}

// cubic (Catmull-Rom) interpolation between the last four input frames
void Engine::Resampler::push(const float *in, unsigned frames) {
    out.clear();
    for (unsigned i = 0; i < frames; i++) {
        for (int k = 0; k < 3; k++) {
            hist[k][0] = hist[k + 1][0];
            hist[k][1] = hist[k + 1][1];
        }
        hist[3][0] = in[i * 2];
        hist[3][1] = in[i * 2 + 1];
        // output frames that fall between hist[1] and hist[2]
        while (pos < 1.0) {
            double t = pos;
            for (int c = 0; c < 2; c++) {
                double p0 = hist[0][c], p1 = hist[1][c], p2 = hist[2][c], p3 = hist[3][c];
                double v = p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
                out.push_back((float)v);
            }
            pos += step;
        }
        pos -= 1.0;
    }
}

void Engine::midi_loop() {
    realtime_priority();
    std::unique_lock<std::mutex> lock(midi_mu_);
    while (run_) {
        if (midi_q_.empty()) {
            midi_cv_.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }
        auto it = std::min_element(midi_q_.begin(), midi_q_.end(), [](const Timed &a, const Timed &b) { return a.at < b.at; });
        if (it->at > clk::now()) {
#ifdef _WIN32
            // Windows wakes up to a millisecond or two late: the last 2 ms are waited out here (the clock's
            // pulses and the pedal's presses have to land on time)
            if (it->at - clk::now() < std::chrono::milliseconds(2)) {
                auto at = it->at;
                lock.unlock();
                while (clk::now() < at) std::this_thread::yield();
                lock.lock();
                continue;
            }
            midi_cv_.wait_until(lock, std::min(it->at - std::chrono::milliseconds(2), clk::now() + std::chrono::milliseconds(50)));
#else
            midi_cv_.wait_until(lock, std::min(it->at, clk::now() + std::chrono::milliseconds(50)));
#endif
            continue;
        }
        std::vector<uint8_t> b = std::move(it->bytes);
        midi_q_.erase(it);
        lock.unlock();
        if (tracing() && !b.empty() && b[0] != 0xF8) {
            Status st = status();
            char hex[64] = "";
            for (size_t i = 0; i < b.size() && i < 8; i++) snprintf(hex + i * 3, 4, "%02X ", b[i]);
            trace(std::string("MIDI ") + hex + "| bar " + std::to_string(st.bar) + " beat " + std::to_string(st.beat) + " sec " + std::to_string(st.section) +
                  " next " + std::to_string(st.next_section) + " track " + std::to_string(st.playing_track) + " pedal '" + st.pedal_state + "' loop '" + st.loop + "'");
        }
        if (midi_.is_open() && !midi_.send(b.data(), b.size())) {
            std::lock_guard<std::mutex> l2(mu_);
            status_.problem = "The pedal's MIDI went away (unplugged?).";
            midi_ok_ = false;
        }
        lock.lock();
    }
}

// ----------------------------------------------------------------------------- commands

void Engine::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mu_);
    commands_.push_back({std::move(fn)});
}

void Engine::run_commands() {
    std::vector<Command> cmds;
    {
        std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);   // never wait in the audio thread
        if (!lock.owns_lock()) return;
        cmds.swap(commands_);
    }
    for (auto &c : cmds) c.fn();
}

void Engine::set_settings(const Settings &s) {
    post([this, s] {
        double now_bpm = set_.bpm;
        set_ = s;
        // the pedal's mode (firmware 1.0.1 and later take the mode notes; older ones are set by hand)
        if (s.pedal_mode >= 1 && s.pedal_mode <= 4 && s.pedal_mode != sent_mode_ && !loop_action_) {
            static const pedal::Cmd M[] = {pedal::MODE_SINGLE, pedal::MODE_MULTI, pedal::MODE_SYNC, pedal::MODE_SONG};
            static const char *N[] = {"Single", "Multi", "Sync", "Song"};
            sent_mode_ = s.pedal_mode;
            uint8_t ch = midi_ch();
            send(frame_, {(uint8_t)(0x90 | ch), (uint8_t)M[s.pedal_mode - 1], 127}, std::string("Pedal: ") + N[s.pedal_mode - 1] + " mode");
            send(frame_ + kit::RATE / 50, {(uint8_t)(0x80 | ch), (uint8_t)M[s.pedal_mode - 1], 0}, "");
            if (log) log(std::string("Pedal set to ") + N[s.pedal_mode - 1] + " mode");
        }
        double want = std::max(30.0, std::min(300.0, s.bpm));
        set_.bpm = now_bpm;
        if (grid_ && playing_) target_bpm_ = want;   // playing: glide there
        else {
            target_bpm_ = want;
            apply_bpm(want);
        }
    });
}

// a new tempo from here on: the bar in progress keeps its place, and a loop being recorded still
// ends after exactly its bars (at the new tempo)
void Engine::apply_bpm(double bpm) {
    bpm = std::max(30.0, std::min(300.0, bpm));
    if (bar_frames_ > 0 && style_) {
        double old = bar_frames_;
        double done = (double)(frame_ - bar_start_) / bar_frames_;
        bar_frames_ = 60.0 / bpm * style_->quarters * kit::RATE;
        bar_start_ = frame_ - (int64_t)(done * bar_frames_);
        if (loop_action_ && old > 0) {
            double r = bar_frames_ / old;
            if (frame_ < loop_start_) {
                loop_start_ = frame_ + (int64_t)((loop_start_ - frame_) * r);
                loop_len_ *= r;
            } else {
                double end_old = (double)loop_start_ + loop_len_;
                loop_len_ = (double)(frame_ - loop_start_) + (end_old - (double)frame_) * r;
            }
            int64_t lead = (int64_t)(set_.lead_ms / 1000.0 * kit::RATE);
            int64_t end = loop_start_ + (int64_t)std::llround(loop_len_);
            std::string tr = loop_track_ ? "B" : "A", other = loop_track_ ? "A" : "B";
            for (auto &p : pending_) {
                bool on = !p.bytes.empty() && (p.bytes[0] & 0xF0) == 0x90;
                if (p.what == "#close" + tr) p.at = end - lead + (on ? 0 : kit::RATE / 50);
                else if (p.what == "#close" + other && frame_ < loop_start_) p.at = loop_start_ - lead + (on ? 0 : kit::RATE / 50);
                else if (frame_ < loop_start_ && (p.what == "Record " + tr || p.what == "Overdub " + tr)) p.at = std::max(frame_, loop_start_ - lead);
            }
        }
    }
    set_.bpm = bpm;
}

Settings Engine::settings() {
    std::lock_guard<std::mutex> lock(mu_);
    return status_settings_;
}

bool Engine::set_kit(const std::shared_ptr<kit::Kit> &k) {
    if (!k) return false;
    post([this, k] {
        kit_ = k;
        for (auto &v : voices_) v.fade = std::min(v.fade ? v.fade : 400, 400);   // let the old kit's sounds die away
    });
    return true;
}

void Engine::set_style(const std::string &id) {
    const pattern::Style *s = pattern::find(id);
    if (!s) return;
    post([this, s] {
        if (playing_ && s != style_) {
            // a new beat starts on the next bar line, in its verse
            next_style_ = s;
        } else {
            style_ = s;
            bar_frames_ = 60.0 / set_.bpm * style_->quarters * kit::RATE;
        }
    });
}

void Engine::play() {
    post([this] {
        if (playing_ && !ending_) return;
        if (ending_ && ending_loops_) drop_switch();   // Play during the ending: the loops keep going (if their Stop hasn't gone)
        ending_ = stop_after_bar_ = ending_loops_ = false;
        clap_ = pause_next_bar_ = half_next_bar_ = false;
        if (!grid_) {
            // nothing running yet: start the bar grid now, with a count-in
            start_grid();
            counting_ = set_.count_in > 0;
            count_bars_left_ = set_.count_in;
            drums_from_next_bar_ = false;
            playing_ = true;
            section_ = pattern::VERSE;
            part_bar_ = -1;
            start_bar(frame_);
        } else {
            // loops may be playing in time with the grid: come in on the next bar line
            stop_grid_at_ = -1;
            drums_from_next_bar_ = true;
            section_ = pattern::VERSE;
            part_bar_ = -1;
        }
    });
}

void Engine::next_part() {
    post([this] {
        if (!playing_ || ending_) return;
        next_section_ = section_ == pattern::VERSE ? pattern::CHORUS : pattern::VERSE;
    });
}

void Engine::go_to(int section) {
    post([this, section] {
        if (!playing_ || ending_) return;
        if (section != section_) next_section_ = section;
    });
}

void Engine::end() {
    post([this] {
        if (!playing_ || ending_) return;
        ending_ = true;
        // the ending starts on the next bar line not yet decided; the loops stop with its last hit
        if (!loop_action_ && grid_) loops_end_with(bar_start_ + (int64_t)std::llround(bar_frames_), 0);
    });
}

// The beat (from the ending's start) where its last hit lands, or the beat after when that hit is
// between beats: the loops stop there, with the final crash, not at the end of its bar.
int Engine::ending_stop_beat() const {
    int last = -1;
    for (const auto &h : style_->parts[pattern::ENDING].hits) last = std::max(last, h.step);
    if (last < 0) return 0;   // an ending with no hits: the loops stop where it starts
    return (int)std::ceil((double)last * style_->quarters / style_->steps - 1e-9);
}

// the ending started at ending_start and is bars_in bars along: the loops' Stop goes for its last hit
// (a beat already gone by: this bar line)
void Engine::loops_end_with(int64_t ending_start, int bars_in) {
    ending_loops_ = true;
    int beat = std::max(0, ending_stop_beat() - bars_in * style_->quarters);
    int64_t line = ending_start + (int64_t)std::llround(beat * bar_frames_ / style_->quarters);
    end_loops(line, beat % style_->quarters == 0);
}

void Engine::pause(bool half) {
    post([this, half] {
        if (!playing_ || counting_ || ending_ || pause_next_bar_ || half_next_bar_) return;
        if (!half) {
            pause_next_bar_ = true;
            return;
        }
        // always the next measure (its second half), like everything else that waits for a bar line
        half_next_bar_ = true;
    });
}

void Engine::clap(int on) {
    post([this, on] {
        if (!playing_ && on != 0) return;
        clap_ = on < 0 ? !clap_ : on == 1;
    });
}

void Engine::audition(int section) {
    post([this, section] {
        audition_ = section >= 0 && section < pattern::SECTIONS ? section : -1;
        part_bar_ = -1;
    });
}

// a hand on the cymbals: everything ringing dies away in about 10 ms
void Engine::grab() {
    for (auto &v : voices_) v.fade = v.fade ? std::min(v.fade, 400) : 400;
}

void Engine::stop_now() {
    post([this] {
        playing_ = counting_ = ending_ = stop_after_bar_ = drums_from_next_bar_ = ending_loops_ = false;
        next_section_ = -1;
        fill_bar_ = false;
        clap_ = pause_next_bar_ = half_next_bar_ = false;
        // the loops stop too, on the next bar line, so they come back in time (a stopped loop picks up
        // where it stopped); the clock runs to there, so the pedal takes the Stop on that downbeat
        if (!loop_action_ && grid_) {
            int64_t bar_line = bar_press_time() + lead();
            end_loops(bar_line);
            if (!set_.keep_time) stop_grid_at_ = bar_line + (int64_t)(bar_frames_ / style_->quarters);
        } else if (!loop_action_)
            end_loops(frame_ + lead());   // (a recording in progress keeps the clock and its loop)
        grab();
    });
}

void Engine::tap_tempo() {
    post([this] {
        int64_t now = frame_;
        if (!taps_.empty() && now - taps_.back() > 2 * kit::RATE) taps_.clear();
        taps_.push_back(now);
        if (taps_.size() > 5) taps_.erase(taps_.begin());
        if (taps_.size() >= 3) {
            double span = (double)(taps_.back() - taps_.front()) / (double)(taps_.size() - 1) / kit::RATE;
            double bpm = std::round(60.0 / span);
            target_bpm_ = bpm;
            apply_bpm(bpm);
            if (log) log("Tempo " + std::to_string((int)bpm) + " (tapped)");
        }
    });
}

void Engine::hit(int note, int vel) {
    post([this, note, vel] { trigger(note, vel, 0); });
}

void Engine::press(pedal::Cmd c, Quant q) {
    post([this, c, q] { schedule(c, q, pedal::name(c)); });
}

void Engine::press_at_bar(int note, double offset_ms) {
    post([this, note, offset_ms] {
        uint8_t ch = midi_ch();
        int64_t lead = (int64_t)(set_.lead_ms / 1000.0 * kit::RATE);
        int64_t at = bar_press_time() + lead + (int64_t)(offset_ms / 1000.0 * kit::RATE);
        send(at, {(uint8_t)(0x90 | ch), (uint8_t)note, 127}, "test note " + std::to_string(note));
        send(at + kit::RATE / 50, {(uint8_t)(0x80 | ch), (uint8_t)note, 0}, "");
        if (log) log("Test: note " + std::to_string(note) + " at the next bar line " + std::to_string((int)offset_ms) + " ms");
    });
}

void Engine::load_slot(int slot) {
    post([this, slot] {
        uint8_t ch = midi_ch();
        // bank 0/0, then the program (the guide: MSB 000, LSB 000)
        send(frame_, {(uint8_t)(0xB0 | ch), 0, 0, (uint8_t)(0xB0 | ch), 32, 0, (uint8_t)(0xC0 | ch), (uint8_t)std::max(0, std::min(127, slot))},
             "Load slot " + std::to_string(slot));
    });
}

void Engine::record_loop(int bars, int track) {
    post([this, bars, track] {
        if (!grid_) {   // nothing running: count in first, then record
            start_grid();
            counting_ = true;
            count_bars_left_ = std::max(1, set_.count_in);
            playing_ = false;
            part_bar_ = -1;
            start_bar(frame_);
        }
        begin_loop(1, bars, track);
    });
}

void Engine::record_through(int bars_a, int bars_b) {
    post([this, bars_a, bars_b] {
        if (set_.pedal_mode == 1) {
            if (log) log("Record A then B needs track B: set the pedal to Song mode (Settings)");
            return;
        }
        if (loop_action_) {
            if (log) log("A loop is already being recorded");
            return;
        }
        if (has_loop_[0] == 1 || has_loop_[1] == 1) {   // on a full track, a press would layer onto it
            if (log) log("Record A then B is for new loops: clear A and B first");
            return;
        }
        if (!grid_) {   // nothing running: count in first
            start_grid();
            counting_ = true;
            count_bars_left_ = std::max(1, set_.count_in);
            playing_ = false;
            part_bar_ = -1;
            start_bar(frame_);
        }
        begin_loop(1, bars_a, 0);
        begin_loop(1, bars_b, 1);   // starts the moment A closes
        back_to_a_ = song_like();
        a_after_b_ = false;
        if (log) log("Recording A (" + std::to_string(bars_a) + ") then B (" + std::to_string(bars_b) + ")" + (back_to_a_ ? ", then A plays" : ""));
    });
}

void Engine::overdub(int bars, int track) {
    post([this, bars, track] {
        if (!grid_) return;
        begin_loop(2, bars, track);
    });
}

// When the pedal starts: always on a bar line, and the loop is exactly `bars` bars, so the pedal's
// loops stay on the bar lines with the drums. The press goes lead_ms early (the pedal waits for its
// downbeat), so a bar line too close to make that early press waits for the next one: a press that
// reaches the pedal after its downbeat would start a beat late and leave the loop a beat off.
//   Another press on the same track while it records: it closes at the next bar line it can make.
//   A press on the other track while one records: that one starts the moment this one closes.
void Engine::begin_loop(int action, int bars, int track) {
    stop_grid_at_ = -1;   // keep time for the loop
    int64_t lead = (int64_t)(set_.lead_ms / 1000.0 * kit::RATE), margin = kit::RATE / 200;   // + 5 ms to spare
    uint8_t ch = midi_ch();
    std::string tr = track ? "B" : "A";
    // the first bar line from `from` that leaves time for the early press
    auto bar_line = [&](int64_t from) {
        double k = std::ceil((double)(from - bar_start_) / bar_frames_ - 1e-9);
        int64_t t = bar_start_ + (int64_t)std::llround(k * bar_frames_);
        while (t - lead < frame_ + margin) t += (int64_t)std::llround(bar_frames_);
        return t;
    };
    if (loop_action_) {
        int64_t cur_end = loop_start_ + (int64_t)std::llround(loop_len_);
        std::string cur = loop_track_ ? "B" : "A";
        if (track == loop_track_) {
            if (frame_ < loop_start_) {
                if (log) log(cur + " is already waiting to start");
                return;
            }
            // close it early, on a bar line (still whole bars)
            int64_t end = std::max(bar_line(frame_), loop_start_ + (int64_t)std::llround(bar_frames_));
            if (end < cur_end) {
                loop_bars_ = std::max(1, (int)std::llround((double)(end - loop_start_) / bar_frames_));
                loop_len_ = (double)(end - loop_start_);
                for (auto &p : pending_)
                    if (p.what == "#close" + cur) p.at = end - lead + ((p.bytes[0] & 0xF0) == 0x90 ? 0 : kit::RATE / 50);
                if (log) log("Closing " + cur + " early, on the next bar (" + std::to_string(loop_bars_) + (loop_bars_ == 1 ? " bar)" : " bars)"));
            }
            return;
        }
    }
    int64_t start;
    bool after = false;
    prev_start_ = -1;
    if (loop_action_ && track != loop_track_) {   // the other track goes on as this one closes
        start = loop_start_ + (int64_t)std::llround(loop_len_);
        // Multi reaches B by selecting it, which would take this track's closing press along with it:
        // the other track starts a bar later, after that press has landed
        if (set_.pedal_mode == 2) start += (int64_t)std::llround(bar_frames_);
        after = true;
        prev_start_ = loop_start_;
        prev_bars_ = loop_bars_;
        prev_action_ = loop_action_;
        if (loop_action_ == 1 || loop_action_ == 2) has_loop_[loop_track_] = 1;   // it closes before this one starts
    } else if (counting_) start = bar_start_ + (int64_t)std::llround(bar_frames_ * (count_bars_left_ + 1));   // after the count-in
    else start = bar_line(frame_);
    loop_action_ = action;
    loop_track_ = track;
    loop_bars_ = std::max(1, bars);
    loop_start_ = start;
    loop_len_ = loop_bars_ * bar_frames_;
    loop_ = std::string(action == 1 ? "Recording " : "Overdub ") + tr + (after ? " when the other track closes" : " on the next bar");
    (void)ch;
    int64_t on = std::max(frame_, start - lead), end = start + (int64_t)std::llround(loop_len_);
    press_track(track, on, (action == 1 ? "Record " : "Overdub ") + tr);
    // ...and let go exactly `bars` bars later (then it plays); "#close" so a tempo change can move it
    press_track(track, end - lead, "#close" + tr);
    if (log) log(std::string(action == 1 ? "Record " : "Overdub ") + tr + (after ? " (after the other track, " : " (") + std::to_string(loop_bars_) +
                 (loop_bars_ == 1 ? " bar)" : " bars)"));
}

// one press of a pedal note: let go 20 ms later. A "#..." press (a loop's close, moved by tempo
// changes) tags its let-go too, so both move together.
void Engine::tap(uint8_t note, int64_t at, const std::string &what) {
    at = std::max(frame_, at);
    send(at, {(uint8_t)(0x90 | midi_ch()), note, 127}, what);
    send(at + kit::RATE / 50, {(uint8_t)(0x80 | midi_ch()), note, 0}, what.rfind("#", 0) == 0 ? what : "");
}

// Record/Play (11), Stop (7), and Clear (49) work on the track the pedal has selected (its own feet can
// change that): a press for that track selects it 10 ms before. B's own notes (Sync/Song) need none.
void Engine::select_before(int track, int64_t at, const std::string &what) {
    if (set_.pedal_mode != 1) tap(track ? pedal::SELECT_B : pedal::SELECT_A, at - kit::RATE / 100, what.rfind("#", 0) == 0 ? what : "");
}

// A Record/Play press for one track. Single mode has one track (A). Multi: select the track, then the
// Record/Play pedal (the guide's B notes work in Sync and Song only). Sync, Song, or not set: B's own
// note, A selected first. The press that closes a recording goes to the track already selected, except
// in Multi, where a handoff may have selected the other one.
void Engine::press_track(int track, int64_t at, const std::string &what) {
    if (set_.pedal_mode == 1) track = 0;
    bool closing = what.rfind("#close", 0) == 0;
    if (track == 1 && b_notes()) {
        tap(pedal::REC_PLAY_B, at, what);
        return;
    }
    if (set_.pedal_mode == 2 || !closing) select_before(track, at, what);
    tap(pedal::REC_PLAY, at, what);
}

void Engine::stop_track(int track, int64_t at, const std::string &what) {
    if (track == 1 && b_notes()) {
        tap(pedal::STOP_B, at, what);
        return;
    }
    select_before(track, at, what);
    tap(pedal::STOP, at, what);
}

// A loop switch waiting for its bar line is taken back (Stop Loops, End, or another pick): its presses
// (all tagged "#play") are dropped. If its Stop already went (a switch that waits a bar), the loop
// is stopped now, on a bar line.
void Engine::drop_switch() {
    if (switch_at_ < 0 || frame_ >= switch_at_) return;
    if (switch_press_at_ >= 0 && frame_ >= switch_press_at_) return;   // its press already went: it's happening
    // only presses that haven't started: a let-go always goes (a note held down on the pedal is a
    // long press, and a long Record/Play is Undo, a long Stop is Clear)
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending &p) {
        return (p.what == "#play" || p.what == "Stop loops") && !p.bytes.empty() && (p.bytes[0] & 0xF0) == 0x90; }), pending_.end());
    if (switch_stop_at_ >= 0 && frame_ >= switch_stop_at_) {
        playing_track_ = -1;
        stopped_[0] = stopped_[1] = true;
    } else
        playing_track_ = shown_track_;
    pedal_state_ = playing_track_ < 0 ? "stopped" : std::string("playing ") + (playing_track_ ? "B" : "A");
    switch_at_ = switch_stop_at_ = switch_press_at_ = -1;
}

void Engine::clear_track(int track) {
    post([this, track] {
        select_before(track, frame_ + kit::RATE / 100);
        tap(pedal::CLEAR, frame_ + kit::RATE / 100);
        if (log) log(set_.pedal_mode == 1 ? "Clear" : track ? "Clear B" : "Clear A");
    });
    cleared(track);
}

int64_t Engine::bar_press_time() {
    int64_t at = bar_start_ + (int64_t)std::llround(bar_frames_);
    if (frame_ < bar_start_) at = bar_start_;
    while (at - lead() < frame_ + kit::RATE / 200) at += (int64_t)std::llround(bar_frames_);
    return at - lead();
}

// Play a track on a bar line. Measured on the pedal (firmware 2.0) with a one-hit, one-bar loop:
//  - Play on a stopped loop waits for the clock's downbeat too (sent 22, 30, or 40 ms early, it came
//    back in the same place), so it goes play_early_ms (the lead) early like the other presses; the
//    pedal then starts it a steady ~17 ms after its own clock-start position, every time;
//  - a stopped loop resumes from where it stopped: stopped on a bar line (Barline's Stop Loops),
//    it comes back on the bar; stopped mid-bar (by foot), it comes back off the grid;
//  - a Stop and a Play close together on a loop that's playing confuse it (it overdubs or goes quiet):
//    a playing loop (or one Barline isn't sure about) stops on the bar line and plays on the next.
// In Song mode, switching to the other track needs no Stop: the pedal stops the one playing.
// Returns whether it has to wait a bar (stop first).
bool Engine::restart_track(int track, int64_t bar_line, const std::string &what) {
    drop_switch();
    bool waits = !song_switch_to(track) && !(stopped_[track] && playing_track_ != track);
    int64_t play_at = bar_line;
    if (waits) {
        stop_track(set_.pedal_mode == 1 ? 0 : track, bar_line - lead(), "#play");   // the pedal stops it on the bar line...
        play_at = bar_line + (int64_t)std::llround(bar_frames_);                 // ...and plays it on the next
    }
    switch_press_at_ = std::max(frame_, play_at - (int64_t)(set_.play_early_ms / 1000.0 * kit::RATE));
    press_track(track, switch_press_at_, "#play");
    begin_switch(track, play_at);
    switch_stop_at_ = waits ? bar_line - lead() : -1;
    if (log && !what.empty()) log(what + (waits ? " (stops on the next bar, plays on the one after)" : " on the next bar"));
    has_loop_[track] = 1;
    stopped_[0] = stopped_[1] = false;
    playing_track_ = track;
    pedal_state_ = std::string("playing ") + (track ? "B" : "A");
    return waits;
}

void Engine::play_track(int track) {
    post([this, track] {
        std::string tr = track ? "B" : "A";
        if (has_loop_[track] == 0) {
            if (log) log("Track " + tr + " is empty (a press there would start recording)");
            return;
        }
        if (!grid_) {   // no clock running: right away
            press_track(track, frame_, "Play " + tr);
            has_loop_[track] = 1;
            playing_track_ = track;
            switch_at_ = -1;
            pedal_state_ = "playing " + tr;
            if (log) log("Play " + tr);
            return;
        }
        // picked again while a switch waits: back to the loop that's playing, nothing more
        if (switch_at_ >= 0 && frame_ < switch_at_ && shown_track_ == track) {
            drop_switch();
            if (log) log("Staying on " + tr);
            return;
        }
        restart_track(track, bar_press_time() + lead(), "Play " + tr);
    });
}

// what the screen shows as playing changes when the pedal does (on that bar line), not when it's asked
void Engine::begin_switch(int to, int64_t at) {
    if (switch_at_ < 0 || frame_ >= switch_at_) shown_track_ = playing_track_;
    switch_to_ = to;
    switch_at_ = at;
}

void Engine::cleared(int track) {
    post([this, track] {
        if (track == 0 || track == 1) has_loop_[track] = 0;   // Barline cleared it: empty
        else has_loop_[0] = has_loop_[1] = -1;               // a new slot: not known
        if (track < 0 || track == playing_track_) playing_track_ = -1;
    });
}

void Engine::update_loop() {
    if (!loop_action_) return;
    std::string tr = loop_track_ ? "B" : "A";
    int64_t end = loop_start_ + (int64_t)std::llround(loop_len_);
    // Record A then B: A's Play goes just after B's closing press, before the same downbeat (Song mode
    // switches to it there, as it does from a playing loop)
    if (back_to_a_ && loop_track_ == 1 && frame_ >= loop_start_ && frame_ >= end - lead() - kit::RATE / 20) {
        back_to_a_ = false;
        a_after_b_ = true;
        press_track(0, std::max(frame_, end - lead() / 2), "Then A");
    }
    if (frame_ >= end) {
        has_loop_[loop_track_] = 1;
        playing_track_ = a_after_b_ && loop_track_ == 1 ? 0 : loop_track_;
        a_after_b_ = false;
        tr = playing_track_ ? "B" : "A";
        stopped_[0] = stopped_[1] = false;
        loop_action_ = 0;
        loop_.clear();
        pedal_state_ = "playing " + tr;
        // no drums: the clock stops a beat after the loop closes
        if (!playing_ && !counting_ && !set_.keep_time && grid_) stop_grid_at_ = end + (int64_t)(bar_frames_ / style_->quarters);
        return;
    }
    if (frame_ < loop_start_ && prev_start_ >= 0 && frame_ >= prev_start_) {   // the other track first, then this one
        std::string pt = loop_track_ ? "A" : "B";
        double bar_len = (double)(loop_start_ - prev_start_) / std::max(1, prev_bars_);
        int bar = std::min(prev_bars_, 1 + (int)((double)(frame_ - prev_start_) / std::max(1.0, bar_len)));
        loop_ = std::string(prev_action_ == 1 ? "Recording " : "Overdubbing ") + pt + ", bar " + std::to_string(bar) + " of " + std::to_string(prev_bars_) + " · " + tr + " next";
        pedal_state_ = std::string(prev_action_ == 1 ? "recording " : "overdubbing ") + pt;
    } else if (frame_ < loop_start_) {   // a countdown to the bar line it starts on
        int beats = (int)std::ceil((double)(loop_start_ - frame_) / (bar_frames_ / style_->quarters) - 1e-9);
        loop_ = std::string(loop_action_ == 1 ? "Recording " : "Overdub ") + tr + " in " + std::to_string(std::max(1, beats));
    } else {
        int bar = 1 + (int)((double)(frame_ - loop_start_) / std::max(1.0, loop_len_ / loop_bars_));
        loop_ = std::string(loop_action_ == 1 ? "Recording " : "Overdubbing ") + tr + ", bar " + std::to_string(std::min(bar, loop_bars_)) + " of " +
                std::to_string(loop_bars_);
        pedal_state_ = loop_action_ == 1 ? "recording " + tr : "overdubbing " + tr;
    }
}

// Stop every loop, on this line (the drums' ending). With the clock running, the pedal takes a Stop on
// its next downbeat, so the presses go a little early. Single: its one track. Song: the one playing
// (not known: Stop, and Stop B). Multi and Sync can play both at once: both, A's a moment before B's
// (each Stop goes to the track selected just before it).
void Engine::end_loops(int64_t line, bool on_bar) {
    drop_switch();
    int64_t at = line - lead();
    if (set_.pedal_mode == 1) stop_track(0, at, "#play");
    else if (set_.pedal_mode == 2 || set_.pedal_mode == 3) {
        stop_track(0, at - kit::RATE / 50, "#play");
        stop_track(1, at, "#play");
    } else if (playing_track_ >= 0) stop_track(playing_track_, at, "#play");
    else {
        tap(pedal::STOP, at, "#play");
        if (b_notes()) tap(pedal::STOP_B, at + kit::RATE / 100, "#play");
    }
    begin_switch(-1, line);
    switch_press_at_ = std::max(frame_, at - kit::RATE / 50);
    switch_stop_at_ = -1;
    // stopped on a bar line, a loop comes back in time; between beats of a bar, Play starts it over
    stopped_[0] = stopped_[1] = on_bar;
    playing_track_ = -1;
    pedal_state_ = "stopped";
    if (log) log("The loops stop with the drums");
}

void Engine::stop_loops(Quant q) {
    post([this, q] {
        back_to_a_ = a_after_b_ = false;
        loop_action_ = 0;
        loop_.clear();
        drop_switch();
        if (q == BAR && grid_) stopped_[0] = stopped_[1] = true;   // stopped on a bar line: Play comes back in time
        if (playing_track_ >= 0 || switch_at_ >= 0) {
            begin_switch(-1, next_boundary(q));
            switch_press_at_ = std::max(frame_, next_boundary(q) - lead());
            switch_stop_at_ = -1;
        }
        playing_track_ = -1;
        schedule(pedal::STOP, q, "Stop loops");
    });
}

void Engine::cancel_loop() {
    post([this] {
        back_to_a_ = a_after_b_ = false;
        loop_action_ = 0;
        loop_.clear();
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Pending &p) { return !p.what.empty(); }), pending_.end());   // (#close too)
    });
}

void Engine::panic() {
    post([this] {
        pending_.clear();
        back_to_a_ = a_after_b_ = false;
        loop_action_ = 0;
        loop_.clear();
        playing_ = counting_ = ending_ = ending_loops_ = false;
        clap_ = pause_next_bar_ = half_next_bar_ = false;
        grid_ = false;
        voices_.clear();
        uint8_t ch = midi_ch();
        send(frame_, {0xFC}, "");
        send(frame_, {(uint8_t)(0xB0 | ch), 123, 0}, "");
    });
}

Status Engine::status() {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

// ----------------------------------------------------------------------------- timing

double Engine::step_frames() const { return bar_frames_ / style_->steps; }

int64_t Engine::step_time(int64_t bar_start, int step) const {
    double L = step_frames();
    double t = step * L;
    double swing = set_.swing >= 0 ? set_.swing : style_->swing;
    // swing pushes every second step late (toward a triplet feel); triplet grids are already swung
    if (style_->steps % 3 && (step & 1)) t += L / 3.0 * swing;
    return bar_start + (int64_t)std::llround(t);
}

int64_t Engine::next_boundary(Quant q) const {
    if (q == NOW || !grid_) return frame_;
    // a press while a clean stop is coming: the grid keeps going for it
    double beat = bar_frames_ / style_->quarters;
    double into = (double)(frame_ - bar_start_);
    if (into < 0) return bar_start_;   // just before a bar line: that one
    if (q == BEAT) return bar_start_ + (int64_t)std::llround((std::floor(into / beat) + 1) * beat);
    return bar_start_ + (int64_t)std::llround(bar_frames_);
}

void Engine::send(int64_t at, const std::vector<uint8_t> &bytes, const std::string &what) {
    pending_.push_back({at, bytes, what});
}

void Engine::schedule(pedal::Cmd c, Quant q, const std::string &what) {
    uint8_t ch = midi_ch();
    int64_t at = next_boundary(q);
    if (q != NOW && grid_) {
        // a moment early: the pedal then waits for the downbeat. Too close to make that? The next one
        // (a press the pedal gets after its downbeat would land a beat late).
        int64_t lead = (int64_t)(set_.lead_ms / 1000.0 * kit::RATE);
        double step = q == BEAT ? bar_frames_ / style_->quarters : bar_frames_;
        while (at - lead < frame_ + kit::RATE / 200) at += (int64_t)std::llround(step);
        at -= lead;
    }
    if (at < frame_) at = frame_;
    send(at, {(uint8_t)(0x90 | ch), (uint8_t)c, 127}, what);
    send(at + kit::RATE / 50, {(uint8_t)(0x80 | ch), (uint8_t)c, 0}, "");   // let go 20 ms later
    if (log && !what.empty()) log(what + (q == BAR ? " on the next bar" : q == BEAT ? " on the next beat" : ""));
}

void Engine::stop_grid() {
    grid_ = false;
    stop_grid_at_ = -1;
    counting_ = false;
    bar_kind_ = -1;
    bar_ = 0;
    if (log) log("Stopped (the clock stops too)");
}

void Engine::start_grid() {
    stop_grid_at_ = -1;
    grid_ = true;
    bar_frames_ = 60.0 / set_.bpm * style_->quarters * kit::RATE;
    bar_start_ = frame_;
    bar_ = 0;
    clock_next_ = frame_;
    clock_frac_ = 0;
    clock_div_now_ = set_.clock_div;
    if (set_.send_clock && set_.midi_start) send(frame_, {0xFA}, "");   // MIDI Start: beat one is now (see Settings::midi_start)
    clock_started_ = true;
}

// ----------------------------------------------------------------------------- bars

void Engine::start_bar(int64_t at) {
    if (style_ && next_style_ && !counting_) {
        style_ = next_style_;
        next_style_ = nullptr;
        part_bar_ = -1;
    }
    auto bars_of = [&](int sec) { return std::max(1, style_->parts[sec].bars); };
    int prev = bar_kind_;
    bar_start_ = at;
    bar_frames_ = 60.0 / set_.bpm * style_->quarters * kit::RATE;
    // the clock's rate changes here, on the bar line, with its pulses starting from it
    if (clock_div_now_ != set_.clock_div) {
        clock_div_now_ = set_.clock_div;
        clock_next_ = at;
        clock_frac_ = 0;
    }
    bar_++;
    bar_kind_ = -1;   // nothing this bar
    // a pause last bar: back in with a crash
    if (paused_) {
        paused_ = false;
        crash_next_ = true;
    }
    silent_bar_ = false;
    mute_from_ = 1 << 30;
    grabbed_ = false;
    if (counting_) {
        if (count_bars_left_ > 0) {   // a count-in bar
            count_bars_left_--;
            kind_bar_ = prev == pattern::INTRO ? kind_bar_ + 1 : 0;
            bar_kind_ = pattern::INTRO;
            goto loops;
        }
        counting_ = false;
    }
    if (drums_from_next_bar_) {
        drums_from_next_bar_ = false;
        playing_ = true;
        part_bar_ = -1;
    }
    if (playing_ && audition_ >= 0) {   // the beat editor: one part on repeat, a transition with its groove before it
        int from = audition_ == pattern::FILL_VERSE || audition_ == pattern::ENDING ? pattern::VERSE
                 : audition_ == pattern::FILL_CHORUS ? pattern::CHORUS : -1;
        int cycle = (from >= 0 ? bars_of(from) : 0) + bars_of(audition_);
        int i = (part_bar_ + 1) % cycle;
        part_bar_ = i;
        if (from >= 0 && i < bars_of(from)) {
            bar_kind_ = from;
            kind_bar_ = i;
        } else {
            bar_kind_ = audition_;
            kind_bar_ = i - (from >= 0 ? bars_of(from) : 0);
            if (from >= 0 && kind_bar_ == 0) crash_next_ = false;
        }
        if (from >= 0 && i == 0) crash_next_ = true;   // back into the groove
        goto loops;
    }
    if (playing_) {
        if (stop_after_bar_) {
            // (End came while a loop was recording: its Stop goes now)
            if (!ending_loops_ && !loop_action_) end_loops(bar_start_);
            playing_ = ending_ = stop_after_bar_ = ending_loops_ = false;
            clap_ = false;
            // the ending is over: stop the clock a beat later (a pedal press waiting for this downbeat gets it)
            if (!set_.keep_time && !loop_action_) stop_grid_at_ = bar_start_ + (int64_t)(bar_frames_ / style_->quarters);
        } else if (ending_) {   // the ending (one bar or two), then stop
            pause_next_bar_ = half_next_bar_ = false;
            kind_bar_ = prev == pattern::ENDING ? kind_bar_ + 1 : 0;
            bar_kind_ = pattern::ENDING;
            // the loops end with the ending's last hit (End came while a loop was recording: once it's closed)
            if (!ending_loops_ && !loop_action_) loops_end_with(bar_start_ - (int64_t)std::llround(kind_bar_ * bar_frames_), kind_bar_);
            if (kind_bar_ + 1 >= bars_of(pattern::ENDING)) stop_after_bar_ = true;
        } else if (clap_) {   // clapping along: the drums wait (a part change waits too)
            kind_bar_ = prev == pattern::CLAP ? (kind_bar_ + 1) % bars_of(pattern::CLAP) : 0;
            bar_kind_ = pattern::CLAP;
            crash_next_ = false;   // (no crash under the claps)
        } else if (prev == pattern::CLAP) {   // the claps are over: the drums come back in
            part_bar_ = 0;
            crash_next_ = true;
            bar_kind_ = section_;
        } else if (fill_bar_ && (prev == pattern::FILL_VERSE || prev == pattern::FILL_CHORUS) && kind_bar_ + 1 < bars_of(prev)) {
            kind_bar_++;   // a two-bar fill: its second bar
            bar_kind_ = prev;
        } else if (fill_bar_) {
            fill_bar_ = false;
            section_ = next_section_ >= 0 ? next_section_ : section_;
            next_section_ = -1;
            part_bar_ = 0;
            crash_next_ = true;
            bar_kind_ = section_;
        } else if (next_section_ >= 0) {
            fill_bar_ = true;
            kind_bar_ = 0;
            bar_kind_ = section_ == pattern::VERSE ? pattern::FILL_VERSE : pattern::FILL_CHORUS;
        } else {
            const pattern::Part &p = style_->parts[section_];
            part_bar_ = (part_bar_ + 1) % std::max(1, p.bars);
            bar_kind_ = section_;
        }
        // Song mode (or not said): Next Part switches the pedal's loop too, to the other track, on the bar
        // line where the new part starts (the last bar of the fill sends it). Only to a track known to hold a
        // loop: a press on an empty track starts a recording, and nobody asked for one here.
        if (song_like() && next_section_ >= 0 && (bar_kind_ == pattern::FILL_VERSE || bar_kind_ == pattern::FILL_CHORUS) &&
            kind_bar_ + 1 >= bars_of(bar_kind_) && !loop_action_) {
            drop_switch();   // a tap still waiting is overtaken: the other track from what's playing now
            int want = playing_track_ >= 0 ? 1 - playing_track_ : next_section_ == pattern::CHORUS ? 1 : 0;
            std::string tr = want ? "B" : "A";
            if (has_loop_[want] == 1) {
                restart_track(want, bar_start_ + (int64_t)std::llround(bar_frames_), "");
                if (log) log("Loop " + tr + " with the " + (next_section_ == pattern::CHORUS ? "chorus" : "verse"));
            }
        }
        // a pause asked for: this bar is silent (or its second half)
        if (pause_next_bar_ && bar_kind_ >= 0) {
            pause_next_bar_ = false;
            silent_bar_ = paused_ = true;
            crash_next_ = false;
        } else if (half_next_bar_ && bar_kind_ >= 0) {
            half_next_bar_ = false;
            mute_from_ = style_->steps / 2;
            paused_ = true;
        }
    }
loops:
    next_step_ = 0;
}

void Engine::trigger(int note, int vel, uint32_t offset) {
    if (!kit_ || vel <= 0) return;
    int n = kit::stand_in(*kit_, note);
    if (n < 0) return;
    const kit::Piece *p = kit_->piece(n);
    if (!p || p->layers.empty()) return;
    const kit::Layer *L = &p->layers.back();
    for (const auto &l : p->layers)
        if (vel >= l.lo && vel <= l.hi) {
            L = &l;
            break;
        }
    if (L->rr.empty()) return;
    unsigned &rr = rr_[(size_t)n];
    auto s = L->rr[rr++ % L->rr.size()];
    // a closed or pedal hi-hat cuts the open one off
    if (p->choke >= 0)
        for (auto &v : voices_)
            if (v.choke == p->choke && !v.fade) v.fade = 220;
    if (voices_.size() >= 48) voices_.erase(voices_.begin());   // the oldest makes room
    Voice v;
    v.keep = s;
    v.s = s.get();
    // softer within the layer; the layer itself already sounds like its loudness
    double g = std::pow((double)vel / L->hi, 1.4) * kit_->gain * set_.volume;
    v.gl = v.gr = (float)g;
    v.choke = p->choke;
    v.start_delay = offset;
    voices_.push_back(std::move(v));
}

void Engine::click(float *b, unsigned frames, unsigned offset, bool accent) {
    (void)b;
    (void)frames;
    click_left_ = kit::RATE / 30;
    click_accent_ = accent;
    click_phase_ = 0;
    click_offset_ = offset;
}

// ----------------------------------------------------------------------------- the audio

void Engine::process(float *out, float *clickb, unsigned frames) {
    run_commands();
    // a tempo change eases in (6 BPM a second) instead of jumping
    if (target_bpm_ > 0 && std::fabs(target_bpm_ - set_.bpm) > 0.01) {
        double step = 6.0 * frames / kit::RATE;
        apply_bpm(set_.bpm + std::max(-step, std::min(step, target_bpm_ - set_.bpm)));
    }
    update_loop();
    std::fill(out, out + (size_t)frames * 2, 0.0f);
    std::fill(clickb, clickb + (size_t)frames * 2, 0.0f);
    int64_t end = frame_ + frames;
    bool click_on = false;
    // when this block reaches the ear (for the MIDI thread)
    auto block_wall = clk::now();
    if (out_.is_open()) block_wall += std::chrono::nanoseconds((int64_t)(1e9 * (double)out_.delay() / kit::RATE));
    if (grid_) {
        // walk through the block event by event: bar lines, steps, clock pulses. Bars and steps are
        // handled AHEAD (by LOOK frames) so a hit can be placed early (a pushed feel) as well as late.
        const int64_t LOOK = kit::RATE / 50;   // 20 ms
        for (int guard = 0; guard < 10000; guard++) {
            int64_t bar_end = bar_start_ + (int64_t)std::llround(bar_frames_);
            const pattern::Part *part = bar_kind_ >= 0 ? &style_->parts[bar_kind_] : nullptr;
            int64_t t_step = INT64_MAX;
            if (part && next_step_ < style_->steps) t_step = step_time(bar_start_, next_step_) - LOOK;
            int64_t t_bar = bar_end - LOOK;
            int64_t t_clock = set_.send_clock ? clock_next_ : INT64_MAX;
            int64_t t_stop = stop_grid_at_ >= 0 ? stop_grid_at_ : INT64_MAX;
            int64_t t = std::min({t_bar, t_step, t_clock, t_stop});
            if (t >= end) break;
            if (t == t_stop && t < t_bar) {
                stop_grid();
                break;
            }
            if (t == t_bar && t < t_step) {
                start_bar(bar_end);
                continue;
            }
            if (t == t_step) {
                int64_t at_time = t + LOOK;   // where the step really is
                // the hits on this step (in a part longer than a bar, this bar's share)
                int which = bar_kind_ == pattern::VERSE || bar_kind_ == pattern::CHORUS ? std::max(0, part_bar_) : kind_bar_;
                int base = (which % std::max(1, part->bars)) * style_->steps;
                int at = base + next_step_;
                std::uniform_real_distribution<double> u(-1, 1);
                bool muted = silent_bar_ || next_step_ >= mute_from_;   // a pause
                if (muted && !grabbed_) {
                    grabbed_ = true;
                    grab();
                }
                if (!muted)
                for (const auto &h : part->hits)
                    if (h.step == at) {
                        int vel = h.vel;
                        double off_ms = 0;
                        if (bar_kind_ != pattern::INTRO) {
                            // the pocket: backbeat and hats sit behind (or ahead of) the beat; the kick stays put
                            int n = h.note;
                            double share = (n >= 37 && n <= 40) ? 1.0 : (n == 42 || n == 44 || n == 46 || n == 51 || n == 53 || n == 54 || n == 69 || n == 70) ? 0.5 : 0.0;
                            off_ms = set_.pocket_ms * share;
                            if (set_.humanize > 0) {
                                vel = std::max(1, std::min(127, vel + (int)std::lround(u(rng_) * 12 * set_.humanize)));
                                if (next_step_) off_ms += u(rng_) * 6.0 * set_.humanize;
                            }
                        }
                        int64_t when = at_time + (int64_t)std::llround(std::max(-19.0, std::min(19.0, off_ms)) / 1000.0 * kit::RATE);
                        trigger(h.note, vel, (uint32_t)std::max<int64_t>(0, when - frame_));
                    }
                if (next_step_ == 0 && crash_next_ && !muted) {
                    trigger(49, 112, (uint32_t)std::max<int64_t>(0, at_time - frame_));
                    crash_next_ = false;
                }
                next_step_++;
                continue;
            }
            // a MIDI clock pulse: 24 a quarter note, or (clock_div) 24 per 2 quarters or per bar, so the pedal's "downbeat"
            // is the bar line and everything it lines up by itself (foot presses too) lands on the bar
            pending_.push_back({t, {0xF8}, ""});
            double pulse = (clock_div_now_ <= 0 ? bar_frames_ / 24.0 : clock_div_now_ * 60.0 / set_.bpm / 24.0 * kit::RATE) + clock_frac_;
            clock_next_ = t + (int64_t)pulse;
            clock_frac_ = pulse - std::floor(pulse);
        }
        // the click: on each beat during a count-in, while a loop is being recorded, or always
        bool want = set_.click && (counting_ || bar_kind_ == pattern::INTRO || loop_action_ || set_.click_always || !loop_.empty());
        if (want) {
            double beat = bar_frames_ / style_->quarters;
            for (int q = 0; q < style_->quarters; q++) {
                int64_t bt = bar_start_ + (int64_t)std::llround(q * beat);
                if (bt >= frame_ && bt < end) click(clickb, frames, (unsigned)(bt - frame_), q == 0);
            }
            click_on = true;
        }
    }
    (void)click_on;
    // the voices
    for (auto &v : voices_) {
        unsigned i = std::min<unsigned>(v.start_delay, frames);
        v.start_delay -= i;
        const int16_t *pcm = v.s->pcm.data();
        uint32_t n = v.s->frames();
        for (; i < frames && v.pos < n; i++, v.pos++) {
            float g = 1;
            if (v.fade) {
                g = (float)v.fade / 400.0f;
                if (--v.fade == 0) {
                    v.pos = n;
                    break;
                }
            }
            out[i * 2] += pcm[v.pos * 2] * (1.0f / 32768.0f) * v.gl * g;
            out[i * 2 + 1] += pcm[v.pos * 2 + 1] * (1.0f / 32768.0f) * v.gr * g;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice &v) { return v.pos >= v.s->frames() && !v.start_delay; }),
                  voices_.end());
    // the click sound: a short beep, higher on beat one
    if (click_left_ > 0) {
        // the click goes to the laptop's own output; into the drums (the audience) only when that's chosen
        float *cb = set_.click_in_drums ? out : clickb;
        double f = click_accent_ ? 1500 : 1000;
        unsigned i = std::min(click_offset_, frames);
        click_offset_ -= i;
        for (; i < frames && click_left_ > 0; i++, click_left_--) {
            double env = (double)click_left_ / (kit::RATE / 30.0);
            float s = (float)(std::sin(click_phase_) * env * env * set_.click_volume * 0.5);
            click_phase_ += 2 * M_PI * f / kit::RATE;
            cb[i * 2] += s;
            cb[i * 2 + 1] += s;
        }
    }
    // soft limit, and the level meter
    float peak = 0;
    for (size_t i = 0; i < (size_t)frames * 2; i++) {
        float x = out[i];
        if (x > 0.9f || x < -0.9f) x = std::tanh(x);
        out[i] = x;
        peak = std::max(peak, std::fabs(x));
    }
    peak_ = std::max((double)peak, peak_ * 0.9);
    // "hear on laptop": the drums on the laptop's own output too (it's off unless you turn it on)
    if (set_.preview)
        for (size_t i = 0; i < (size_t)frames * 2; i++) clickb[i] += out[i];
    // MIDI that's due in this block goes to the MIDI thread, timed to when this block is heard
    if (!pending_.empty()) {
        // stable: at the same moment, one track's close goes before the other's start
        std::stable_sort(pending_.begin(), pending_.end(), [](const Pending &a, const Pending &b) { return a.at < b.at; });
        std::vector<Pending> keep;
        std::vector<Timed> due;
        for (auto &p : pending_) {
            if (p.at >= end) {
                keep.push_back(std::move(p));
                continue;
            }
            double secs = (double)(std::max(p.at, frame_) - frame_) / kit::RATE + set_.latency_ms / 1000.0;
            if (offline_midi_) offline_midi_->push_back({(double)std::max(p.at, frame_) / kit::RATE, p.bytes});
            else due.push_back({block_wall + std::chrono::nanoseconds((int64_t)(secs * 1e9)), p.bytes});
            if (!p.what.empty() && p.what.rfind("Record", 0) == 0) pedal_state_ = "recording";
        }
        pending_.swap(keep);
        if (!due.empty()) {
            std::lock_guard<std::mutex> lock(midi_mu_);
            for (auto &d : due) midi_q_.push_back(std::move(d));
            midi_cv_.notify_all();
        }
    }
    frame_ = end;
    publish();
}

void Engine::publish() {
    std::unique_lock<std::mutex> lock(mu_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    Status &s = status_;
    s.running = true;
    s.playing = playing_;
    s.counting = counting_;
    s.bar = bar_;
    s.beats = style_->quarters;
    double into = bar_frames_ > 0 ? (double)(frame_ - bar_start_) / bar_frames_ : 0;
    s.beat = grid_ ? 1 + std::min(style_->quarters - 1, (int)(into * style_->quarters)) : 0;
    s.phase = grid_ ? std::max(0.0, std::min(1.0, into * style_->quarters - (s.beat - 1))) : 0;
    s.section = section_;
    s.next_section = next_section_;
    s.ending = ending_;
    s.clapping = clap_;
    s.clap_now = bar_kind_ == pattern::CLAP;
    s.pause = silent_bar_ || (mute_from_ < style_->steps && next_step_ >= mute_from_) ? 2 : pause_next_bar_ || half_next_bar_ || mute_from_ < style_->steps ? 1 : 0;
    s.bpm = set_.bpm;
    s.style = style_->name;
    s.kit = kit_ ? kit_->name : "";
    s.loop = loop_;
    s.pedal_state = pedal_state_;
    s.has_a = has_loop_[0] == 1;
    s.has_b = has_loop_[1] == 1;
    s.empty_a = has_loop_[0] == 0;
    s.empty_b = has_loop_[1] == 0;
    bool switching = switch_at_ >= 0 && frame_ < switch_at_;
    s.playing_track = switching ? shown_track_ : playing_track_;
    s.next_track = switching ? (switch_to_ < 0 ? 2 : switch_to_) : -1;
    s.xruns = out_.xruns;
    s.level = peak_;
    s.grid = grid_;
    s.out_ok = out_ok_;
    s.midi_ok = midi_ok_;
    status_settings_ = set_;
}

void Engine::render(size_t frames, std::vector<float> &out, std::vector<MidiOut> *midi) {
    offline_midi_ = midi;
    unsigned n = 256;
    buf_.assign((size_t)n * 2, 0);
    click_buf_.assign((size_t)n * 2, 0);
    out.clear();
    for (size_t done = 0; done < frames; done += n) {
        process(buf_.data(), click_buf_.data(), n);
        for (unsigned i = 0; i < n * 2; i++) out.push_back(buf_[i] + (set_.click_in_drums ? 0 : click_buf_[i]));
    }
    offline_midi_ = nullptr;
}

void Engine::render_split(size_t frames, std::vector<float> &to_pedal, std::vector<float> &to_laptop) {
    unsigned n = 256;
    buf_.assign((size_t)n * 2, 0);
    click_buf_.assign((size_t)n * 2, 0);
    to_pedal.clear();
    to_laptop.clear();
    for (size_t done = 0; done < frames; done += n) {
        process(buf_.data(), click_buf_.data(), n);
        to_pedal.insert(to_pedal.end(), buf_.begin(), buf_.end());
        to_laptop.insert(to_laptop.end(), click_buf_.begin(), click_buf_.end());
    }
}

}  // namespace engine
