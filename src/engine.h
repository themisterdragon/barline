// The live engine: one audio thread that plays the drums, counts bars, sends MIDI clock, and
// presses the pedal's buttons for you exactly on the bar line. Everything is timed in samples,
// so the drums, the clock, and the pedal agree.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kit.h"
#include "pattern.h"
#include "pedal.h"
#include "snd.h"

namespace engine {

// BARLINE_TRACE=<file>: every iPad/keyboard action and every MIDI message sent, with CLOCK_MONOTONIC ms (for diagnosing)
void trace(const std::string &s);
bool tracing();

// When a pedal press lands
enum Quant { NOW, BEAT, BAR };

struct Devices {
    int out_card = -1, out_dev = 0;       // drums (the pedal's USB input, or any card); -1 = no sound out
    int click_card = -1, click_dev = 0;   // the click, for headphones; -1 = none (or mixed in, see Settings)
    int midi_card = -1, midi_dev = 0;     // the pedal's MIDI in; -1 = none
    unsigned period = 128;                // frames per block (128 = 2.9 ms)
};

struct Settings {
    double bpm = 100;
    double swing = -1;          // -1 = the style's own
    double humanize = 0.25;     // 0..1: a little timing and loudness drift, like a person
    double volume = 0.8;        // drums
    double click_volume = 0.6;
    bool click = true;          // a click during the count-in and while recording loops
    bool click_always = false;  // ...or all the time
    bool click_in_drums = false;   // the click goes out with the drums (no separate headphone output)
    int count_in = 1;           // bars of count before the beat starts (0-2)
    int midi_channel = 1;       // the pedal's (1-16; it can also be set to Omni)
    bool send_clock = true;
    // MIDI Start (FA) when the clock starts. Off: on the pedal (firmware 2.0) Start plays a stopped loop
    // by itself, so the next song's drums would bring the last song's loop back. The pedal keeps to the
    // beat from the clock alone (checked: loops still come in within ~5 ms of the bar line).
    bool midi_start = false;
    int clock_div = 1;          // quarter notes per pedal beat in the MIDI clock (1 normal; 2 half bars; 0 = a whole bar)
    double lead_ms = 30;        // pedal presses go this early, so the pedal catches the downbeat
    double play_early_ms = 30;  // Play on a stopped loop goes this early (it waits for the downbeat too)
    double latency_ms = 0;      // MIDI goes this much later, to land with the drums as the pedal hears them
                                // (measured on the pedal: its downbeats came 25 ms ahead of the USB drums)
    bool keep_time = false;     // keep the bars (and clock) going after the drums stop
    bool preview = false;       // the drums also play on the laptop's own sound (only when you say so)
    double grace_beats = 0;     // (no longer used: a loop always starts on a bar line)
    // the pedal's looper mode: 0 = not said, 1 Single, 2 Multi, 3 Sync, 4 Song. Barline sets the pedal to
    // it (firmware 1.0.1+ takes the mode notes; on 1.0.0 set it on the pedal too, Menu > Mode).
    // Track B needs Multi, Sync, or Song; in Multi, B is reached by selecting it first (the B notes are
    // for Sync and Song only, per the guide).
    int pedal_mode = 0;
    double pocket_ms = 0;       // the feel: snare and hats this much behind (+, laid back) or ahead (-, pushed); kick stays put
};

struct Status {
    bool running = false, playing = false, counting = false;
    int bar = 0;                // bars since the beat started (from 1)
    int beat = 0;               // 1..beats in the bar
    int beats = 4;
    double phase = 0;           // how far into that beat (0..1), so a screen can move its lights smoothly
    int section = pattern::VERSE;
    int next_section = -1;      // a change waiting for the bar line
    bool ending = false;
    bool clapping = false;      // the clap-along is on (or starts at the next bar line)
    bool clap_now = false;      // ...and the claps are playing this bar
    int pause = 0;              // 0 none, 1 waiting for its moment, 2 silent now
    double bpm = 100;
    std::string style, kit;
    // the loop macro in progress: "Recording A, bar 2 of 4"
    std::string loop;
    int loop_bars_left = 0;
    // what Barline believes the pedal is doing (it can't ask)
    std::string pedal_state;
    bool has_a = false, has_b = false;       // the tracks Barline knows hold a loop
    bool empty_a = false, empty_b = false;   // ...and the ones it knows are empty (it cleared them)
    int playing_track = -1;              // Song mode: the track playing (-1 none or unknown)
    int next_track = -1;                 // ...a switch waiting for the next bar line: 0 A, 1 B, 2 the loops stop
    int xruns = 0;
    double level = 0;           // output peak, 0..1
    std::string problem;        // a device went away, etc.
    bool out_ok = false, midi_ok = false;   // the drums' sound device and the pedal's MIDI are open and working
    bool grid = false;          // the bar grid (and MIDI clock) is running
};

class Engine {
public:
    Engine();
    ~Engine();
    // live: opens the devices and starts the audio thread. It keeps running (on a timer) even when a
    // device won't open; then it's false, and err says what's missing.
    bool start(const Devices &d, std::string *err);
    void stop();
    // a device came or went (the pedal plugged in, unplugged, or back): the same beat, bars, and
    // loops carry on with the new devices
    bool reopen(const Devices &d, std::string *err);
    // the devices asked for are all open and working
    bool healthy() const { return out_ok_ == want_out_ && midi_ok_ == want_midi_; }
    // the laptop's own output (the click, "hear on laptop") was asked for but isn't open (busy, say)
    bool laptop_missing() const { return laptop_wanted_ && has_laptop() && !click_ok_; }
    // the laptop's own sound on (the click or "hear on laptop" is on) or off (then the computer's other
    // sound can have it). Opened right away, with no gap in the drums to the pedal.
    bool set_laptop(bool want, std::string *err = nullptr);
    bool has_laptop() const { return has_laptop_; }   // there's a laptop output separate from the drums
    // offline (tests, previews): render frames into stereo floats, recording MIDI it would send
    struct MidiOut {
        double at;   // seconds from the start
        std::vector<uint8_t> bytes;
    };
    void render(size_t frames, std::vector<float> &out, std::vector<MidiOut> *midi = nullptr);
    // the same, with the pedal's output and the laptop's (click and "hear on laptop") kept apart
    void render_split(size_t frames, std::vector<float> &to_pedal, std::vector<float> &to_laptop);

    // ---- what you can do (from any thread)
    void set_settings(const Settings &s);
    Settings settings();
    bool set_kit(const std::shared_ptr<kit::Kit> &k);
    void set_style(const std::string &id);
    void play();                 // count in, intro, then the verse
    void next_part();            // fill next bar, then the other part
    void go_to(int section);     // VERSE / CHORUS, after a fill
    void end();                  // the ending on the next bar, then stop
    // a dramatic pause: the drums stop (cymbals grabbed) and come back on the next downbeat with a
    // crash. half: the second half of a bar (this one if there's still time, else the next);
    // whole: the whole next bar. The bars and the clock carry on, so loops stay in time.
    void pause(bool half);
    // clap along: from the next bar line the drums drop out and hand claps play (the beat's clap
    // part), until it's turned off; then the drums come back on the next bar with a crash
    void clap(int on);           // 1 on, 0 off, -1 flip
    // the beat editor: play this part (with the groove it comes out of, for a transition) on repeat;
    // -1 = the normal way
    void audition(int section);
    void stop_now();
    void tap_tempo();
    void hit(int note, int vel);   // play one drum now (pads)
    // the pedal
    void press(pedal::Cmd c, Quant q = NOW);
    // tests: one pedal note exactly offset_ms from the next bar line (no lead)
    void press_at_bar(int note, double offset_ms);
    void load_slot(int slot);    // Program Change
    // record a loop exactly `bars` long on track A (0) or B (1): Record on the next bar line, then
    // Play exactly that many bars later. The pedal's Logic must be set to R>P>D.
    void record_loop(int bars, int track = 0);
    // add a layer for `bars` (one or more loop cycles): Overdub on the next bar line, then back to Play
    void overdub(int bars, int track = 0);
    // one press for a whole song's loops: A (bars_a) on the next bar line, B (bars_b) the moment A
    // closes, then A plays the moment B closes (Song mode; Multi and Sync play both). No waiting
    // in between for the audience.
    void record_through(int bars_a, int bars_b);
    void stop_loops(Quant q = BAR);
    void cancel_loop();
    // play the loop on track A (0) or B (1), on the next bar line (Song mode: the other one stops).
    // Nothing happens if that track has no loop (a press there would start recording).
    void play_track(int track);
    void clear_track(int track);   // Clear on that track (selected first: Clear works on the pedal's selected track)
    void cleared(int track);     // a track was cleared (-1 = both, e.g. a new slot)
    void panic();                // all notes off, clock stop, pending presses dropped

    Status status();
    std::function<void(const std::string &)> log;   // what happened, for the screen

private:
    struct Voice {
        const kit::Sample *s = nullptr;
        std::shared_ptr<kit::Sample> keep;
        uint32_t pos = 0;
        float gl = 0, gr = 0;
        int choke = -1;
        int fade = 0;   // frames left of a quick fade (choked)
        uint32_t start_delay = 0;
    };
    struct Pending {
        int64_t at;          // the frame it's due
        std::vector<uint8_t> bytes;
        std::string what;
    };
    struct Command {
        std::function<void()> fn;
    };

    // audio thread state (only touched there, or under mu_ before start)
    std::shared_ptr<kit::Kit> kit_, next_kit_;
    const pattern::Style *style_ = nullptr, *next_style_ = nullptr;
    bool grid_ = false;            // bars are being counted (and clock sent), drums or not
    bool drums_from_next_bar_ = false;
    int bar_kind_ = -1;            // the part this bar plays (-1 = silence)
    int kind_bar_ = 0;             // which bar of a fill / ending / clap / count-in part this is
    bool clap_ = false;            // the clap-along is wanted (takes effect at bar lines)
    bool pause_next_bar_ = false, half_next_bar_ = false;   // a pause waiting for the next bar
    bool silent_bar_ = false;      // this whole bar is a pause
    int mute_from_ = 1 << 30;      // ...or this bar is silent from this step on
    bool grabbed_ = false;         // the cymbals were grabbed for this pause
    bool paused_ = false;          // this bar had a pause: come back with a crash
    int audition_ = -1;            // the beat editor's part on repeat
    void grab();                   // stop what's ringing, quickly (a hand on the cymbals)
    int next_step_ = 0;
    std::array<unsigned, 128> rr_{};
    int loop_bars_ = 0;
    int64_t loop_start_ = 0;       // the frame the pedal starts recording (exact)
    double loop_len_ = 0;          // and how long it records, in frames (exactly loop_bars_ bars)
    void begin_loop(int action, int bars, int track);
    void update_loop();
    double target_bpm_ = 0;        // a tempo change glides here
    void apply_bpm(double bpm);
    std::string pending_state_;
    unsigned click_offset_ = 0;
    Settings status_settings_;
    void start_grid();
    void stop_grid();
    bool closing_ = false;         // a loop's closing press was just sent
    int64_t stop_grid_at_ = -1;    // the bars and clock stop here (a clean stop), -1 = not stopping
    Settings set_;
    std::vector<Voice> voices_;
    int64_t frame_ = 0;            // frames since the engine started
    bool playing_ = false, counting_ = false, ending_ = false, stop_after_bar_ = false;
    int section_ = pattern::VERSE, next_section_ = -1;
    bool fill_bar_ = false;        // this bar is a fill
    int has_loop_[2] = {-1, -1};   // per track: 1 has a loop, 0 empty (Barline cleared it), -1 not known
    int playing_track_ = -1;
    bool stopped_[2] = {false, false};   // stopped by Barline on a bar line (so Play can come straight back in)
    int sent_mode_ = 0;            // the mode last sent to the pedal
    // a Record/Play press for a track at `at` (Multi mode selects the track first)
    void press_track(int track, int64_t at, const std::string &what);
    bool restart_track(int track, int64_t bar_line, const std::string &what);   // from the top, on that bar line (true: waits a bar)
    void tap(uint8_t note, int64_t at, const std::string &what = "");
    void select_before(int track, int64_t at, const std::string &what = "");
    void stop_track(int track, int64_t at, const std::string &what = "");
    void drop_switch();
    void begin_switch(int to, int64_t at);
    uint8_t midi_ch() const { return (uint8_t)(std::max(1, std::min(16, set_.midi_channel)) - 1); }
    int64_t lead() const { return (int64_t)(set_.lead_ms / 1000.0 * kit::RATE); }
    bool song_like() const { return set_.pedal_mode == 4 || set_.pedal_mode == 0; }   // Song, or not said
    bool b_notes() const { return set_.pedal_mode == 0 || set_.pedal_mode == 3 || set_.pedal_mode == 4; }   // B's own notes work
    bool song_switch_to(int track) const { return song_like() && playing_track_ >= 0 && playing_track_ != track; }
    int shown_track_ = -1, switch_to_ = -1;   // the screen: the loop playing now, and the switch waiting for
    int64_t switch_at_ = -1;                  // ...this frame (-1 none; switch_to_ -1 = the loops stop)
    int64_t switch_stop_at_ = -1;             // a switch that waits a bar: when its Stop goes
    int64_t switch_press_at_ = -1;            // when its Play (or the loops' Stop) goes: after that it can't be taken back
    void end_loops(int64_t line, bool on_bar = true);   // every loop stops on that line (the drums' ending)
    int ending_stop_beat() const;                      // the beat of the ending's last hit
    void loops_end_with(int64_t ending_start, int bars_in);
    bool ending_loops_ = false;                        // the ending's loop Stop is already on its way
    int64_t bar_press_time();      // when a press for the next bar line goes (early enough for the pedal)
    bool crash_next_ = false;
    int64_t bar_start_ = 0;        // frame the current bar started
    int bar_ = 0, count_bars_left_ = 0, part_bar_ = 0;
    double bar_frames_ = 0;
    int64_t clock_next_ = 0;       // next MIDI clock pulse
    double clock_frac_ = 0;
    int clock_div_now_ = 1;        // the clock's rate in use (it changes only on a bar line, so the pedal's
                                   // downbeats stay on bar lines)
    bool clock_started_ = false;
    std::vector<Pending> pending_;
    std::string loop_;
    int loop_left_ = 0, loop_action_ = 0, loop_track_ = 0;
    int64_t prev_start_ = -1;       // a recording handed off to the other track: when it started, how long
    int prev_bars_ = 0, prev_action_ = 0;
    bool back_to_a_ = false;        // record_through: A plays when B closes (its press not sent yet)
    bool a_after_b_ = false;        // ...sent: A is what plays when B closes
    std::string pedal_state_ = "unknown";
    std::mt19937 rng_{12345};
    std::vector<int64_t> taps_;
    double peak_ = 0;
    std::vector<MidiOut> *offline_midi_ = nullptr;

    std::mutex mu_;                // commands and status
    std::vector<Command> commands_;
    Status status_;
    std::atomic<bool> run_{false};
    std::atomic<bool> out_ok_{false}, midi_ok_{false}, click_ok_{false};
    std::atomic<bool> want_out_{false}, want_midi_{false}, has_laptop_{false};   // what the devices in use ask for
    void shut(bool goodbye);
    std::thread thread_;
    snd::Pcm out_;
    std::shared_ptr<snd::Pcm> click_;   // the laptop's own sound, while it's wanted (only the audio thread swaps it)
    std::atomic<bool> laptop_wanted_{false};
    std::mutex laptop_mu_, retire_mu_;
    std::vector<std::shared_ptr<snd::Pcm>> retired_;
    std::shared_ptr<snd::Pcm> laptop_leak_;
    void retire(std::shared_ptr<snd::Pcm> p);
    static bool open_at(snd::Pcm &p, int card, int devn, unsigned periods, unsigned period, std::string *err);
    snd::Midi midi_;
    Devices dev_;
    std::vector<float> buf_, click_buf_;
    // a device that won't take 44.1 kHz (some built-in sound) gets the sound converted to its rate
    struct Resampler {
        double step = 1, pos = 0;   // input frames per output frame
        float hist[4][2] = {};      // the last four input frames
        std::vector<float> out;
        void push(const float *in, unsigned frames);
    } out_rs_, click_rs_;

    // MIDI goes out from its own thread at the right moment
    struct Timed {
        std::chrono::steady_clock::time_point at;
        std::vector<uint8_t> bytes;
    };
    std::mutex midi_mu_;
    std::vector<Timed> midi_q_;
    std::thread midi_thread_;
    std::condition_variable_any midi_cv_;

    void post(std::function<void()> fn);
    void loop_thread();
    void midi_loop();
    void process(float *out, float *click, unsigned frames);
    void run_commands();
    void start_bar(int64_t at);
    void trigger(int note, int vel, uint32_t offset);
    void click(float *buf, unsigned frames, unsigned offset, bool accent);
    void send(int64_t at, const std::vector<uint8_t> &bytes, const std::string &what = "");
    void schedule(pedal::Cmd c, Quant q, const std::string &what);
    int64_t next_boundary(Quant q) const;
    double step_frames() const;
    int64_t step_time(int64_t bar_start, int step) const;
    void publish();
    // the click sound
    double click_phase_ = 0;
    int click_left_ = 0;
    bool click_accent_ = false;
};

}  // namespace engine
